/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 2017 The ProFTPD Project team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, The ProFTPD Project team and other respective
 * copyright holders give permission to link this program with OpenSSL, and
 * distribute the resulting executable, without including the source code for
 * OpenSSL in the source distribution.
 */

/* Redis management */

#include "conf.h"

#ifdef PR_USE_REDIS

#include <hiredis/hiredis.h>

struct redis_rec {
  pool *pool;
  module *owner;
  redisContext *ctx;

  /* For tracking the number of "opens"/"closes" on a shared redis_rec,
   * as the same struct might be used by multiple modules in the same
   * session, each module doing a conn_get()/conn_close().
   */
  unsigned int refcount;

  /* Table mapping modules to their namespaces */
  pr_table_t *namespace_tab;
};

static const char *redis_server = NULL;
static int redis_port = -1;

static pr_redis_t *sess_redis = NULL;

static unsigned long redis_connect_millis = 500;
static unsigned long redis_io_millis = 500;

static const char *trace_channel = "redis";

static void millis2timeval(struct timeval *tv, unsigned long millis) {
  tv->tv_sec = (millis / 1000);
  tv->tv_usec = (millis - (tv->tv_sec * 1000)) * 1000;
}

static const char *redis_strerror(pool *p, pr_redis_t *redis, int rerrno) {
  const char *err;

  switch (redis->ctx->err) {
    case REDIS_ERR_IO:
      err = pstrcat(p, "[io] ", strerror(rerrno), NULL);
      break;

    case REDIS_ERR_EOF:
      err = pstrcat(p, "[eof] ", redis->ctx->errstr, NULL);
      break;

    case REDIS_ERR_PROTOCOL:
      err = pstrcat(p, "[protocol] ", redis->ctx->errstr, NULL);
      break;

    case REDIS_ERR_OOM:
      err = pstrcat(p, "[oom] ", redis->ctx->errstr, NULL);
      break;

    case REDIS_ERR_OTHER:
      err = pstrcat(p, "[other] ", redis->ctx->errstr, NULL);
      break;

    case REDIS_OK:
    default:
      err = "OK";
      break;
  }

  return err;
}

static int ping_server(pr_redis_t *redis) {
  const char *cmd;
  redisReply *reply;

  cmd = "PING";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s", cmd);
  if (reply == NULL) {
    int xerrno;
    pool *tmp_pool;

    xerrno = errno;
    tmp_pool = make_sub_pool(redis->pool);
    pr_trace_msg(trace_channel, 2, "error sending %s command: %s", cmd,
      redis_strerror(tmp_pool, redis, xerrno));

    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  /* We COULD assert a "PONG" response here, but really, anything is OK. */
  pr_trace_msg(trace_channel, 7, "%s reply: %s", cmd, reply->str);
  freeReplyObject(reply);
  return 0;
}

static int stat_server(pr_redis_t *redis) {
  const char *cmd;
  redisReply *reply;

  cmd = "INFO";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s", cmd);
  if (reply == NULL) {
    int xerrno;
    pool *tmp_pool;

    xerrno = errno;
    tmp_pool = make_sub_pool(redis->pool);
    pr_trace_msg(trace_channel, 2, "error sending %s command: %s", cmd,
      redis_strerror(tmp_pool, redis, xerrno));

    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %s", cmd, reply->str);
  freeReplyObject(reply);
  return 0;
}

pr_redis_t *pr_redis_conn_get(pool *p) {
  if (sess_redis != NULL) {
    sess_redis->refcount++;
    return sess_redis;
  }

  return pr_redis_conn_new(p, NULL, 0UL);
}

static int set_conn_options(pr_redis_t *redis, unsigned long flags) {
  int res, xerrno;
  struct timeval tv;
  pool *tmp_pool;

  tmp_pool = make_sub_pool(redis->pool);

  millis2timeval(&tv, redis_io_millis);
  res = redisSetTimeout(redis->ctx, tv);
  if (res == REDIS_ERR) {
    xerrno = errno;

    pr_trace_msg(trace_channel, 4,
      "error setting %lu ms timeout: %s", redis_io_millis,
      redis_strerror(tmp_pool, redis, xerrno));
  }

#if HIREDIS_MAJOR >= 0 && \
    HIREDIS_MINOR >= 12
  res = redisEnableKeepAlive(redis->ctx);
  if (res == REDIS_ERR) {
    xerrno = errno;

    pr_trace_msg(trace_channel, 4,
      "error setting keepalive: %s", redis_strerror(tmp_pool, redis, xerrno));
  }
#endif /* HiRedis 0.12.0 and later */

  destroy_pool(tmp_pool);
  return 0;
}

static void sess_redis_cleanup(void *data) {
  sess_redis = NULL;
}

pr_redis_t *pr_redis_conn_new(pool *p, module *m, unsigned long flags) {
  int uses_ip = TRUE, res, xerrno;
  pr_redis_t *redis;
  pool *sub_pool;
  redisContext *ctx;
  struct timeval tv;

  if (p == NULL) {
    errno = EINVAL;
    return NULL;
  }

  if (redis_server == NULL) {
    pr_trace_msg(trace_channel, 9, "%s",
      "unable to create new Redis connection: No server configured");
    errno = EPERM;
    return NULL;
  }

  millis2timeval(&tv, redis_connect_millis); 

  /* If the given redis "server" string starts with a '/' character, assume
   * that it is a Unix socket path.
   */
  if (*redis_server == '/') {
    uses_ip = FALSE;
    ctx = redisConnectUnixWithTimeout(redis_server, tv);

  } else {
    ctx = redisConnectWithTimeout(redis_server, redis_port, tv);
  }

  xerrno = errno;

  if (ctx == NULL) {
    errno = ENOMEM;
    return NULL;
  }

  if (ctx->err != 0) {
    const char *err_type, *err_msg;

    switch (ctx->err) {
      case REDIS_ERR_IO:
        err_type = "io";
        err_msg = strerror(xerrno);
        break;

      case REDIS_ERR_EOF:
        err_type = "eof";
        err_msg = ctx->errstr;
        break;

      case REDIS_ERR_PROTOCOL:
        err_type = "protocol";
        err_msg = ctx->errstr;
        break;

      case REDIS_ERR_OOM:
        err_type = "oom";
        err_msg = ctx->errstr;
        break;

      case REDIS_ERR_OTHER:
        err_type = "other";
        err_msg = ctx->errstr;
        break;

      default:
        err_type = "unknown";
        err_msg = ctx->errstr;
        break;
    }

    if (uses_ip == TRUE) {
      pr_trace_msg(trace_channel, 3,
        "error connecting to %s#%d: [%s] %s", redis_server, redis_port,
        err_type, err_msg);

    } else {
      pr_trace_msg(trace_channel, 3,
        "error connecting to '%s': [%s] %s", redis_server, err_type, err_msg);
    }

    redisFree(ctx);
    errno = EIO;
    return NULL;
  }

  sub_pool = make_sub_pool(p);
  pr_pool_tag(sub_pool, "Redis connection pool");

  redis = pcalloc(sub_pool, sizeof(pr_redis_t));
  redis->pool = sub_pool;
  redis->owner = m;
  redis->ctx = ctx;
  redis->refcount = 1;

  /* The namespace table is null; it will be created if/when callers
   * configure namespace prefixes.
   */
  redis->namespace_tab = NULL;

  /* Set some of the desired behavior flags on the connection */
  res = set_conn_options(redis, flags);
  if (res < 0) {
    xerrno = errno;

    pr_redis_conn_destroy(redis);
    errno = xerrno;
    return NULL;    
  }

  res = ping_server(redis);
  if (res < 0) {
    xerrno = errno;

    pr_redis_conn_destroy(redis);
    errno = xerrno;
    return NULL;
  }

  /* Make sure we are connected to the configured server by querying
   * some stats/info from it.
   */
  res = stat_server(redis);
  if (res < 0) {
    xerrno = errno;

    pr_redis_conn_destroy(redis);
    errno = xerrno;
    return NULL;    
  }

  if (sess_redis == NULL) {
    sess_redis = redis;

    /* Register a cleanup on this redis, so that when it is destroyed, we
     * clear this sess_redis pointer, lest it remaining dangling.
     */
    register_cleanup(redis->pool, NULL, sess_redis_cleanup, NULL);
  }

  return redis;
}

int pr_redis_conn_close(pr_redis_t *redis) {
  if (redis == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (redis->refcount > 0) {
    redis->refcount--;
  }

  if (redis->refcount == 0) {
    if (redis->ctx != NULL) {
      const char *cmd = NULL;
      redisReply *reply;

      cmd = "QUIT";
      pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
      reply = redisCommand(redis->ctx, "%s", cmd);
      if (reply != NULL) {
        freeReplyObject(reply);
      }

      redisFree(redis->ctx);
      redis->ctx = NULL;
    }

    if (redis->namespace_tab != NULL) {
      (void) pr_table_empty(redis->namespace_tab);
      (void) pr_table_free(redis->namespace_tab);
      redis->namespace_tab = NULL;
    }
  }

  return 0;
}

int pr_redis_conn_destroy(pr_redis_t *redis) {
  int res;

  if (redis == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_conn_close(redis);
  if (res < 0) {
    return -1;
  }

  destroy_pool(redis->pool);
  return 0;
}

int pr_redis_conn_clone(pool *p, pr_redis_t *redis) {
  /* This is a no-op, for now. */
  return 0;
}

static int modptr_cmp_cb(const void *k1, size_t ksz1, const void *k2,
    size_t ksz2) {

  /* Return zero to indicate a match, non-zero otherwise. */
  return (((module *) k1) == ((module *) k2) ? 0 : 1);
}

static unsigned int modptr_hash_cb(const void *k, size_t ksz) {
  unsigned int key = 0;

  /* XXX Yes, this is a bit hacky for "hashing" a pointer value. */

  memcpy(&key, k, sizeof(key));
  key ^= (key >> 16);

  return key;
}

int pr_redis_conn_set_namespace(pr_redis_t *redis, module *m,
    const char *prefix) {

  if (redis == NULL ||
      m == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (redis->namespace_tab == NULL) {
    pr_table_t *tab;

    tab = pr_table_alloc(redis->pool, 0);

    (void) pr_table_ctl(tab, PR_TABLE_CTL_SET_KEY_CMP, modptr_cmp_cb);
    (void) pr_table_ctl(tab, PR_TABLE_CTL_SET_KEY_HASH, modptr_hash_cb);
    redis->namespace_tab = tab;
  }

  if (prefix != NULL) {
    int count;
    size_t prefix_len;

    prefix_len = strlen(prefix);

    count = pr_table_kexists(redis->namespace_tab, m, sizeof(module *));
    if (count <= 0) {
      if (pr_table_kadd(redis->namespace_tab, m, sizeof(module *),
          pstrndup(redis->pool, prefix, prefix_len), prefix_len) < 0) {
        pr_trace_msg(trace_channel, 7,
          "error adding namespace prefix '%s' for module 'mod_%s.c': %s",
          prefix, m->name, strerror(errno));
      }

    } else {
      if (pr_table_kset(redis->namespace_tab, m, sizeof(module *),
          pstrndup(redis->pool, prefix, prefix_len), prefix_len) < 0) {
        pr_trace_msg(trace_channel, 7,
          "error setting namespace prefix '%s' for module 'mod_%s.c': %s",
          prefix, m->name, strerror(errno));
      }
    }

  } else {
    /* A NULL prefix means the caller is removing their namespace maping. */
    (void) pr_table_kremove(redis->namespace_tab, m, sizeof(module *), NULL);
  }

  return 0;
}

int pr_redis_add(pr_redis_t *redis, module *m, const char *key, void *value,
    size_t valuesz, time_t expires) {
  int res;

  /* XXX Should we allow null values to be added, thus allowing use of keys
   * as sentinels?
   */
  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      value == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_kadd(redis, m, key, strlen(key), value, valuesz, expires);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error adding key '%s', value (%lu bytes): %s", key,
      (unsigned long) valuesz, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_decr(pr_redis_t *redis, module *m, const char *key, uint32_t decr,
    uint64_t *value) {
  int res;

  /* XXX Should we allow null values to be added, thus allowing use of keys
   * as sentinels?
   */
  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      decr == 0) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_kdecr(redis, m, key, strlen(key), decr, value);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error decrementing key '%s' by %lu: %s", key,
      (unsigned long) decr, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

void *pr_redis_get(pool *p, pr_redis_t *redis, module *m, const char *key,
    size_t *valuesz) {
  void *ptr = NULL;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL ||
      valuesz == NULL) {
    errno = EINVAL;
    return NULL;
  }

  ptr = pr_redis_kget(p, redis, m, key, strlen(key), valuesz);
  if (ptr == NULL) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error getting data for key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return NULL;
  }

  return ptr;
}

char *pr_redis_get_str(pool *p, pr_redis_t *redis, module *m, const char *key) {
  char *ptr = NULL;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL) {
    errno = EINVAL;
    return NULL;
  }

  ptr = pr_redis_kget_str(p, redis, m, key, strlen(key));
  if (ptr == NULL) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error getting data for key '%s': %s", key, strerror(xerrno));

    errno = xerrno; 
    return NULL;
  }

  return ptr;
}

int pr_redis_incr(pr_redis_t *redis, module *m, const char *key, uint32_t incr,
    uint64_t *value) {
  int res;

  /* XXX Should we allow null values to be added, thus allowing use of keys
   * as sentinels?
   */
  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      incr == 0) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_kincr(redis, m, key, strlen(key), incr, value);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error incrementing key '%s' by %lu: %s", key,
      (unsigned long) incr, strerror(xerrno));
 
    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_remove(pr_redis_t *redis, module *m, const char *key) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_kremove(redis, m, key, strlen(key));
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error removing key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_set(pr_redis_t *redis, module *m, const char *key, void *value,
    size_t valuesz, time_t expires) {
  int res;

  /* XXX Should we allow null values to be added, thus allowing use of keys
   * as sentinels?
   */
  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      value == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_kset(redis, m, key, strlen(key), value, valuesz, expires);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error setting key '%s', value (%lu bytes): %s", key,
      (unsigned long) valuesz, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

/* Hash operations */
int pr_redis_hash_count(pr_redis_t *redis, module *m, const char *key,
    uint64_t *count) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      count == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_kcount(redis, m, key, strlen(key), count);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error counting hash using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_hash_delete(pr_redis_t *redis, module *m, const char *key,
    const char *field) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      field == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_kdelete(redis, m, key, strlen(key), field, strlen(field));
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error deleting field from hash using key '%s', field '%s': %s", key,
      field, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_hash_exists(pr_redis_t *redis, module *m, const char *key,
    const char *field) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      field == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_kexists(redis, m, key, strlen(key), field, strlen(field));
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error checking existence of hash using key '%s', field '%s': %s", key,
      field, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return res;
}

int pr_redis_hash_get(pool *p, pr_redis_t *redis, module *m, const char *key,
    const char *field, void **value, size_t *valuesz) {
  int res;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL ||
      field == NULL ||
      value == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_kget(p, redis, m, key, strlen(key), field, strlen(field),
    value, valuesz);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error getting field from hash using key '%s', field '%s': %s", key,
      field, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_hash_getall(pool *p, pr_redis_t *redis, module *m,
    const char *key, pr_table_t **hash) {
  int res;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL ||
      hash == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_kgetall(p, redis, m, key, strlen(key), hash);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error entire hash using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_hash_incr(pr_redis_t *redis, module *m, const char *key,
    const char *field, int32_t incr, int64_t *value) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      field == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_kincr(redis, m, key, strlen(key), field, strlen(field),
    incr, value);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error incrementing field in hash using key '%s', field '%s': %s", key,
      field, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_hash_keys(pool *p, pr_redis_t *redis, module *m, const char *key,
    array_header **fields) {
  int res;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL ||
      fields == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_kkeys(p, redis, m, key, strlen(key), fields);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error obtaining keys from hash using key '%s': %s", key,
      strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_hash_remove(pr_redis_t *redis, module *m, const char *key) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_kremove(redis, m, key, strlen(key));
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error removing hash using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_hash_set(pr_redis_t *redis, module *m, const char *key,
    const char *field, void *value, size_t valuesz) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      field == NULL ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_kset(redis, m, key, strlen(key), field, strlen(field),
    value, valuesz);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error setting field in hash using key '%s', field '%s': %s", key, field,
      strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_hash_setall(pr_redis_t *redis, module *m, const char *key,
    pr_table_t *hash) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      hash == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_ksetall(redis, m, key, strlen(key), hash);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error setting hash using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_hash_values(pool *p, pr_redis_t *redis, module *m,
    const char *key, array_header **values) {
  int res;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL ||
      values == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_hash_kvalues(p, redis, m, key, strlen(key), values);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error getting values of hash using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

/* List operations */
int pr_redis_list_append(pr_redis_t *redis, module *m, const char *key,
    void *value, size_t valuesz) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_list_kappend(redis, m, key, strlen(key), value, valuesz);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error appending to list using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_list_count(pr_redis_t *redis, module *m, const char *key,
    uint64_t *count) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      count == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_list_kcount(redis, m, key, strlen(key), count);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error counting list using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_list_delete(pr_redis_t *redis, module *m, const char *key,
    void *value, size_t valuesz) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_list_kdelete(redis, m, key, strlen(key), value, valuesz);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error deleting item from list using key '%s': %s", key,
      strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_list_exists(pr_redis_t *redis, module *m, const char *key,
    unsigned int idx) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_list_kexists(redis, m, key, strlen(key), idx);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error checking item at index %u in list using key '%s': %s", idx, key,
      strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return res;
}

int pr_redis_list_remove(pr_redis_t *redis, module *m, const char *key) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_list_kremove(redis, m, key, strlen(key));
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error removing list using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_list_set(pr_redis_t *redis, module *m, const char *key,
    unsigned int idx, void *value, size_t valuesz) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_list_kset(redis, m, key, strlen(key), idx, value, valuesz);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error setting item in list using key '%s', index %u: %s", key, idx,
      strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

/* Set operations */
int pr_redis_set_add(pr_redis_t *redis, module *m, const char *key,
    void *value, size_t valuesz) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_set_kadd(redis, m, key, strlen(key), value, valuesz);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error adding item to set using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_set_count(pr_redis_t *redis, module *m, const char *key,
    uint64_t *count) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      count == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_set_kcount(redis, m, key, strlen(key), count);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error counting set using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_set_delete(pr_redis_t *redis, module *m, const char *key,
    void *value, size_t valuesz) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_set_kdelete(redis, m, key, strlen(key), value, valuesz);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error deleting item from set using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

int pr_redis_set_exists(pr_redis_t *redis, module *m, const char *key,
    void *value, size_t valuesz) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_set_kexists(redis, m, key, strlen(key), value, valuesz);
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error checking item in set using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return res;
}

int pr_redis_set_remove(pr_redis_t *redis, module *m, const char *key) {
  int res;

  if (redis == NULL ||
      m == NULL ||
      key == NULL) {
    errno = EINVAL;
    return -1;
  }

  res = pr_redis_set_kremove(redis, m, key, strlen(key));
  if (res < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 2,
      "error removing set using key '%s': %s", key, strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  return 0;
}

static const char *get_namespace_prefix(pr_redis_t *redis, module *m) {
  const char *prefix = NULL;

  if (m != NULL &&
      redis->namespace_tab != NULL) {
    const char *v;

    v = pr_table_kget(redis->namespace_tab, m, sizeof(module *), NULL);
    if (v != NULL) {
      pr_trace_msg(trace_channel, 25,
        "using namespace prefix '%s' for module 'mod_%s.c'", v, m->name);

      prefix = v;
    }
  }

  return prefix;
}

static const char *get_reply_type(redisReply *reply) {
  const char *type_name;

  switch (reply->type) {
    case REDIS_REPLY_STRING:
      type_name = "STRING";
      break;

    case REDIS_REPLY_ARRAY:
      type_name = "ARRAY";
      break;

    case REDIS_REPLY_INTEGER:
      type_name = "INTEGER";
      break;

    case REDIS_REPLY_NIL:
      type_name = "NIL";
      break;

    case REDIS_REPLY_STATUS:
      type_name = "STATUS";
      break;

    case REDIS_REPLY_ERROR:
      type_name = "ERROR";
      break;

    default:
      type_name = "unknown";
  }

  return type_name;
}

int pr_redis_kadd(pr_redis_t *redis, module *m, const char *key, size_t keysz,
    void *value, size_t valuesz, time_t expires) {
  return pr_redis_kset(redis, m, key, keysz, value, valuesz, expires);
}

int pr_redis_kdecr(pr_redis_t *redis, module *m, const char *key, size_t keysz,
    uint32_t decr, uint64_t *value) {
  int xerrno;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      decr == 0) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis DECRBY pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "DECRBY";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %lu", cmd, key, keysz,
    (unsigned long) decr);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error decrementing key (%lu bytes) by %lu using %s: %s",
      (unsigned long) keysz, (unsigned long) decr, cmd,
      redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = EIO;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  /* Note: DECRBY will automatically set the key value to zero if it does
   * not already exist.  To detect a nonexistent key, then, we look to
   * see if the return value is exactly our requested decrement.  If so,
   * REMOVE the auto-created key, and return ENOENT.
   */
  if ((decr * -1) == (uint32_t) reply->integer) {
    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    (void) pr_redis_kremove(redis, m, key, keysz);
    errno = ENOENT;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);

  if (value != NULL) {
    *value = (uint64_t) reply->integer;
  }

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

void *pr_redis_kget(pool *p, pr_redis_t *redis, module *m, const char *key,
    size_t keysz, size_t *valuesz) {
  int xerrno = 0;
  const char *cmd, *namespace_prefix;
  pool *tmp_pool;
  redisReply *reply;
  char *data = NULL;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL ||
      valuesz == NULL) {
    errno = EINVAL;
    return NULL;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis GET pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "GET";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b", cmd, key, keysz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting data for key (%lu bytes) using %s: %s",
      (unsigned long) keysz, cmd, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = EIO;
    return NULL;
  }

  if (reply->type == REDIS_REPLY_NIL) {
    pr_trace_msg(trace_channel, 7, "%s reply: Nil", cmd);
    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = ENOENT;
    return NULL;
  }

  if (reply->type != REDIS_REPLY_STRING) {
    pr_trace_msg(trace_channel, 2,
      "expected STRING reply for %s, got %s", cmd, get_reply_type(reply));
    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return NULL;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %.*s", cmd, (int) reply->len,
    reply->str);

  if (valuesz != NULL) {
    *valuesz = (uint64_t) reply->len;
  }

  data = palloc(p, reply->len);
  memcpy(data, reply->str, reply->len);

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return data;
}

char *pr_redis_kget_str(pool *p, pr_redis_t *redis, module *m, const char *key,
    size_t keysz) {
  int xerrno = 0;
  const char *cmd, *namespace_prefix;
  pool *tmp_pool;
  redisReply *reply;
  char *data = NULL;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL) {
    errno = EINVAL;
    return NULL;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis GET pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "GET";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b", cmd, key, keysz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting data for key (%lu bytes) using %s: %s",
      (unsigned long) keysz, cmd, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = EIO;
    return NULL;
  }

  if (reply->type == REDIS_REPLY_NIL) {
    pr_trace_msg(trace_channel, 7, "%s reply: Nil", cmd);
    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = ENOENT;
    return NULL;
  }

  if (reply->type != REDIS_REPLY_STRING) {
    pr_trace_msg(trace_channel, 2,
      "expected STRING reply for %s, got %s", cmd, get_reply_type(reply));
    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return NULL;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %.*s", cmd, (int) reply->len,
    reply->str);

  data = pstrndup(p, reply->str, reply->len);

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return data;
}

int pr_redis_kincr(pr_redis_t *redis, module *m, const char *key, size_t keysz,
    uint32_t incr, uint64_t *value) {
  int xerrno;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      incr == 0) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis INCRRBY pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "INCRBY";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %lu", cmd, key, keysz,
    (unsigned long) incr);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error incrementing key (%lu bytes) by %lu using %s: %s",
      (unsigned long) keysz, (unsigned long) incr, cmd,
      redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = EIO;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  /* Note: INCRBY will automatically set the key value to zero if it does
   * not already exist.  To detect a nonexistent key, then, we look to
   * see if the return value is exactly our requested increment.  If so,
   * REMOVE the auto-created key, and return ENOENT.
   */
  if (incr == (uint32_t) reply->integer) {
    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    (void) pr_redis_kremove(redis, m, key, keysz);
    errno = ENOENT;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);

  if (value != NULL) {
    *value = (uint64_t) reply->integer;
  }

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_kremove(pr_redis_t *redis, module *m, const char *key,
    size_t keysz) {
  int xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;
  long long count;

  if (redis == NULL ||
      m == NULL ||
      key == NULL) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis DEL pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "DEL";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b", cmd, key, keysz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error removing key (%lu bytes): %s", (unsigned long) keysz,
      redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));
    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);
  count = reply->integer;

  freeReplyObject(reply);
  destroy_pool(tmp_pool);

  if (count == 0) {
    /* No keys removed. */
    errno = ENOENT;
    return -1;
  }

  return 0;
}

int pr_redis_kset(pr_redis_t *redis, module *m, const char *key, size_t keysz,
    void *value, size_t valuesz, time_t expires) {
  int xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  /* XXX Should we allow null values to be added, thus allowing use of keys
   * as sentinels?
   */
  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      value == NULL) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis SET pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  if (expires > 0) {
    cmd = "SETEX";
    pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
    reply = redisCommand(redis->ctx, "%s %b %lu %b", cmd, key, keysz,
      (unsigned long) expires, value, valuesz);

  } else {
    cmd = "SET";
    pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
    reply = redisCommand(redis->ctx, "%s %b %b", cmd, key, keysz, value,
      valuesz);
  }

  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error adding key (%lu bytes), value (%lu bytes) using %s: %s",
      (unsigned long) keysz, (unsigned long) valuesz, cmd,
      redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = EIO;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %s", cmd, reply->str);
  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_hash_kcount(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, uint64_t *count) {
  int xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      count == NULL) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis HLEN pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "HLEN";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b", cmd, key, keysz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting count of hash using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd,
      get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);
  *count = reply->integer;

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_hash_kdelete(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, const char *field, size_t fieldsz) {
  int xerrno = 0, exists = FALSE;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      field == NULL ||
      fieldsz == 0) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis HDEL pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "HDEL";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %b", cmd, key, keysz, field, fieldsz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting count of hash using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd,
      get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);
  exists = reply->integer ? TRUE : FALSE;

  freeReplyObject(reply);
  destroy_pool(tmp_pool);

  if (exists == FALSE) {
    errno = ENOENT;
    return -1;
  }

  return 0;
}

int pr_redis_hash_kexists(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, const char *field, size_t fieldsz) {
  int xerrno = 0, exists = FALSE;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      field == NULL ||
      fieldsz == 0) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis HEXISTS pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "HEXISTS";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %b", cmd, key, keysz, field, fieldsz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting count of hash using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd,
      get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);
  exists = reply->integer ? TRUE : FALSE;

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return exists;
}

int pr_redis_hash_kget(pool *p, pr_redis_t *redis, module *m, const char *key,
    size_t keysz, const char *field, size_t fieldsz, void **value,
    size_t *valuesz) {
  int xerrno = 0, exists = FALSE;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      field == NULL ||
      fieldsz == 0 ||
      value == NULL) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis HGET pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "HGET";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %b", cmd, key, keysz, field, fieldsz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting item for field in hash using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_STRING &&
      reply->type != REDIS_REPLY_NIL) {
    pr_trace_msg(trace_channel, 2,
      "expected STRING or NIL reply for %s, got %s", cmd,
      get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  if (reply->type == REDIS_REPLY_STRING) {
    pr_trace_msg(trace_channel, 7, "%s reply: (%lu bytes)", cmd,
      (unsigned long) reply->len);

    *value = palloc(p, reply->len);
    memcpy(*value, reply->str, reply->len);

    if (valuesz != NULL) {
      *valuesz = reply->len;
    }

    exists = TRUE;

  } else {
    pr_trace_msg(trace_channel, 7, "%s reply: nil", cmd);
  }

  freeReplyObject(reply);
  destroy_pool(tmp_pool);

  if (exists == FALSE) {
    errno = ENOENT;
    return -1;
  }

  return 0;
}

int pr_redis_hash_kgetall(pool *p, pr_redis_t *redis, module *m,
    const char *key, size_t keysz, pr_table_t **hash) {
  int res, xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      hash == NULL) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis HGETALL pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "HGETALL";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b", cmd, key, keysz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting hash using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_ARRAY) {
    pr_trace_msg(trace_channel, 2,
      "expected ARRAY reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  if (reply->elements > 0) {
    register unsigned int i;

    pr_trace_msg(trace_channel, 7, "%s reply: %lu elements", cmd,
      (unsigned long) reply->elements);

    *hash = pr_table_alloc(p, 0);

    for (i = 0; i < reply->elements; i += 2) {
      redisReply *key_elt, *value_elt;
      void *key_data = NULL, *value_data = NULL;
      size_t key_datasz = 0, value_datasz = 0;

      key_elt = reply->element[i];
      if (key_elt->type == REDIS_REPLY_STRING) {
        key_datasz = key_elt->len;
        key_data = palloc(p, key_datasz);
        memcpy(key_data, key_elt->str, key_datasz);

      } else {
        pr_trace_msg(trace_channel, 2,
          "expected STRING element at index %u, got %s", i,
          get_reply_type(key_elt));
      }

      value_elt = reply->element[i+1];
      if (value_elt->type == REDIS_REPLY_STRING) {
        value_datasz = value_elt->len;
        value_data = palloc(p, value_datasz);
        memcpy(value_data, value_elt->str, value_datasz);

      } else {
        pr_trace_msg(trace_channel, 2,
          "expected STRING element at index %u, got %s", i + 2,
          get_reply_type(value_elt));
      }

      if (key_data != NULL &&
          value_data != NULL) {
        if (pr_table_kadd(*hash, key_data, key_datasz, value_data,
            value_datasz) < 0) {
          pr_trace_msg(trace_channel, 2,
            "error adding key (%lu bytes), value (%lu bytes) to hash: %s",
            (unsigned long) key_datasz, (unsigned long) value_datasz,
            strerror(errno));
        }
      }
    }

    res = 0;

  } else {
    xerrno = ENOENT;
    res = -1;
  }

  freeReplyObject(reply);
  destroy_pool(tmp_pool);

  errno = xerrno;
  return res;
}

int pr_redis_hash_kincr(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, const char *field, size_t fieldsz, int32_t incr,
    int64_t *value) {
  int xerrno = 0, exists = FALSE;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      field == NULL ||
      fieldsz == 0) {
    errno = EINVAL;
    return -1;
  }

  exists = pr_redis_hash_kexists(redis, m, key, keysz, field, fieldsz);
  if (exists == FALSE) {
    errno = ENOENT;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis HINCRBY pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "HINCRBY";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %b %d", cmd, key, keysz, field,
    fieldsz, incr);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error incrementing field in hash using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);
  if (value != NULL) {
    *value = (int64_t) reply->integer;
  }

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_hash_kkeys(pool *p, pr_redis_t *redis, module *m, const char *key,
    size_t keysz, array_header **fields) {
  int res, xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      fields == NULL) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis HKEYS pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "HKEYS";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b", cmd, key, keysz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting fields of hash using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_ARRAY) {
    pr_trace_msg(trace_channel, 2,
      "expected ARRAY reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  if (reply->elements > 0) {
    register unsigned int i;

    pr_trace_msg(trace_channel, 7, "%s reply: %lu elements", cmd,
      (unsigned long) reply->elements);

    *fields = make_array(p, reply->elements, sizeof(char *));
    for (i = 0; i < reply->elements; i++) {
      redisReply *elt;

      elt = reply->element[i];
      if (elt->type == REDIS_REPLY_STRING) {
        char *field;

        field = pcalloc(p, reply->len + 1);
        memcpy(field, reply->str, reply->len);
        *((char **) push_array(*fields)) = field;

      } else {
        pr_trace_msg(trace_channel, 2,
          "expected STRING element at index %u, got %s", i,
          get_reply_type(elt));
      }
    }

    res = 0;

  } else {
    xerrno = ENOENT;
    res = -1;
  }

  freeReplyObject(reply);
  destroy_pool(tmp_pool);

  errno = xerrno;
  return res;
}

int pr_redis_hash_kremove(pr_redis_t *redis, module *m, const char *key,
    size_t keysz) {

  /* Note: We can actually use just DEL here. */
  return pr_redis_kremove(redis, m, key, keysz);
}

int pr_redis_hash_kset(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, const char *field, size_t fieldsz, void *value,
    size_t valuesz) {
  int xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      field == NULL ||
      fieldsz == 0 ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis HSET pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "HSET";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %b %b", cmd, key, keysz, field,
    fieldsz, value, valuesz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error setting item for field in hash using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd,
      get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_hash_ksetall(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, pr_table_t *hash) {
  int count, xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  array_header *args, *arglens;
  redisReply *reply;
  const void *key_data;
  size_t key_datasz;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      hash == NULL) {
    errno = EINVAL;
    return -1;
  }

  /* Skip any empty hashes. */
  count = pr_table_count(hash);
  if (count <= 0) {
    pr_trace_msg(trace_channel, 9, "skipping empty table");
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis HMSET pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "HMSET";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);

  args = make_array(tmp_pool, count + 1, sizeof(char *));
  arglens = make_array(tmp_pool, count + 1, sizeof(size_t));

  *((char **) push_array(args)) = pstrdup(tmp_pool, cmd);
  *((size_t *) push_array(arglens)) = strlen(cmd);

  *((char **) push_array(args)) = (char *) key;
  *((size_t *) push_array(arglens)) = keysz;

  pr_table_rewind(hash);
  key_data = pr_table_knext(hash, &key_datasz);
  while (key_data != NULL) {
    const void *value_data;
    size_t value_datasz;

    pr_signals_handle();

    value_data = pr_table_kget(hash, key_data, key_datasz, &value_datasz);
    if (value_data != NULL) {
      char *key_dup, *value_dup;

      key_dup = palloc(tmp_pool, key_datasz);
      memcpy(key_dup, key_data, key_datasz);
      *((char **) push_array(args)) = key_dup;
      *((size_t *) push_array(arglens)) = key_datasz;

      value_dup = palloc(tmp_pool, value_datasz);
      memcpy(value_dup, value_data, value_datasz);
      *((char **) push_array(args)) = value_dup;
      *((size_t *) push_array(arglens)) = value_datasz;
    }

    key_data = pr_table_knext(hash, &key_datasz);
  }

  reply = redisCommandArgv(redis->ctx, args->nelts, args->elts, arglens->elts);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error setting hash using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_STRING &&
      reply->type != REDIS_REPLY_STATUS) {
    pr_trace_msg(trace_channel, 2,
      "expected STRING or STATUS reply for %s, got %s", cmd,
      get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %.*s", cmd, (int) reply->len,
    reply->str);

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_hash_kvalues(pool *p, pr_redis_t *redis, module *m,
    const char *key, size_t keysz, array_header **values) {
  int res, xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (p == NULL ||
      redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      values == NULL) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis HVALS pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "HVALS";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b", cmd, key, keysz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting values of hash using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_ARRAY) {
    pr_trace_msg(trace_channel, 2,
      "expected ARRAY reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  if (reply->elements > 0) {
    register unsigned int i;

    pr_trace_msg(trace_channel, 7, "%s reply: %lu elements", cmd,
      (unsigned long) reply->elements);

    *values = make_array(p, reply->elements, sizeof(char *));
    for (i = 0; i < reply->elements; i++) {
      redisReply *elt;

      elt = reply->element[i];
      if (elt->type == REDIS_REPLY_STRING) {
        char *value;

        value = pcalloc(p, reply->len + 1);
        memcpy(value, reply->str, reply->len);
        *((char **) push_array(*values)) = value;

      } else {
        pr_trace_msg(trace_channel, 2,
          "expected STRING element at index %u, got %s", i,
          get_reply_type(elt));
      }
    }

    res = 0;

  } else {
    xerrno = ENOENT;
    res = -1;
  }

  freeReplyObject(reply);
  destroy_pool(tmp_pool);

  errno = xerrno;
  return res;
}

int pr_redis_list_kappend(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, void *value, size_t valuesz) {
  int xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis RPUSH pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "RPUSH";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %b", cmd, key, keysz, value, valuesz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error appending to list using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_list_kcount(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, uint64_t *count) {
  int xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      count == NULL) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis LLEN pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "LLEN";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b", cmd, key, keysz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting count of list using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);
  *count = (uint64_t) reply->integer;

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_list_kdelete(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, void *value, size_t valuesz) {
  int xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;
  long long count = 0;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis LREM pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "LREM";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b 0 %b", cmd, key, keysz, value,
    valuesz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error deleting item from set using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);
  count = reply->integer;

  freeReplyObject(reply);
  destroy_pool(tmp_pool);

  if (count == 0) {
    /* No keys removed. */
    errno = ENOENT;
    return -1;
  }

  return 0;
}

int pr_redis_list_kexists(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, unsigned int idx) {
  int xerrno = 0, exists = FALSE;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;
  uint64_t count;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0) {
    errno = EINVAL;
    return -1;
  }

  if (pr_redis_list_kcount(redis, m, key, keysz, &count) == 0) {
    if (count > 0 &&
        idx > 0 &&
        idx >= count) {
      pr_trace_msg(trace_channel, 14,
        "request index %u exceeds list length %lu", idx, (unsigned long) count);
      errno = ERANGE;
      return -1;
    }
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis LINDEX pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "LINDEX";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %u", cmd, key, keysz, idx);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting item at index %u of list using key (%lu bytes): %s", idx,
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_STRING &&
      reply->type != REDIS_REPLY_NIL) {
    pr_trace_msg(trace_channel, 2,
      "expected STRING or NIL reply for %s, got %s", cmd,
      get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  exists = reply->type == REDIS_REPLY_STRING ? TRUE : FALSE;

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return exists;
}

int pr_redis_list_kremove(pr_redis_t *redis, module *m, const char *key,
    size_t keysz) {

  /* Note: We can actually use just DEL here. */
  return pr_redis_kremove(redis, m, key, keysz);
}

int pr_redis_list_kset(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, unsigned int idx, void *value, size_t valuesz) {
  int xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis LSET pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "LSET";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %u %b", cmd, key, keysz, idx, value,
    valuesz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error setting item at index %u in list using key (%lu bytes): %s", idx,
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_STRING &&
      reply->type != REDIS_REPLY_STATUS) {
    pr_trace_msg(trace_channel, 2,
      "expected STRING or STATUS reply for %s, got %s", cmd,
      get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %.*s", cmd, (int) reply->len,
    reply->str);

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_set_kadd(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, void *value, size_t valuesz) {
  int xerrno = 0, exists = FALSE;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  exists = pr_redis_set_kexists(redis, m, key, keysz, value, valuesz);
  if (exists == TRUE) {
    errno = EEXIST;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis SADD pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "SADD";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %b", cmd, key, keysz, value, valuesz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error adding to set using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_set_kcount(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, uint64_t *count) {
  int xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      count == NULL) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis SCARD pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "SCARD";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b", cmd, key, keysz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error getting count of set using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);
  *count = (uint64_t) reply->integer;

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return 0;
}

int pr_redis_set_kdelete(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, void *value, size_t valuesz) {
  int xerrno = 0;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;
  long long count = 0;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis SREM pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "SREM";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %b", cmd, key, keysz, value, valuesz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error deleting item from set using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);
  count = reply->integer;

  freeReplyObject(reply);
  destroy_pool(tmp_pool);

  if (count == 0) {
    /* No keys removed. */
    errno = ENOENT;
    return -1;
  }

  return 0;
}

int pr_redis_set_kexists(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, void *value, size_t valuesz) {
  int xerrno = 0, exists = FALSE;
  pool *tmp_pool = NULL;
  const char *cmd = NULL, *namespace_prefix;
  redisReply *reply;

  if (redis == NULL ||
      m == NULL ||
      key == NULL ||
      keysz == 0 ||
      value == NULL ||
      valuesz == 0) {
    errno = EINVAL;
    return -1;
  }

  tmp_pool = make_sub_pool(redis->pool);
  pr_pool_tag(tmp_pool, "Redis SISMEMBER pool");

  namespace_prefix = get_namespace_prefix(redis, m);
  if (namespace_prefix != NULL) {
    key = pstrcat(tmp_pool, namespace_prefix, key, NULL);
  }

  cmd = "SISMEMBER";
  pr_trace_msg(trace_channel, 7, "sending command: %s", cmd);
  reply = redisCommand(redis->ctx, "%s %b %b", cmd, key, keysz, value, valuesz);
  xerrno = errno;

  if (reply == NULL) {
    pr_trace_msg(trace_channel, 2,
      "error checking item in set using key (%lu bytes): %s",
      (unsigned long) keysz, redis_strerror(tmp_pool, redis, xerrno));
    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    pr_trace_msg(trace_channel, 2,
      "expected INTEGER reply for %s, got %s", cmd, get_reply_type(reply));

    if (reply->type == REDIS_REPLY_ERROR) {
      pr_trace_msg(trace_channel, 2, "%s error: %s", cmd, reply->str);
    }

    freeReplyObject(reply);
    destroy_pool(tmp_pool);
    errno = EINVAL;
    return -1;
  }

  pr_trace_msg(trace_channel, 7, "%s reply: %lld", cmd, reply->integer);
  exists = reply->integer ? TRUE : FALSE;

  freeReplyObject(reply);
  destroy_pool(tmp_pool);
  return exists;
}

int pr_redis_set_kremove(pr_redis_t *redis, module *m, const char *key,
    size_t keysz) {

  /* Note: We can actually use just DEL here. */
  return pr_redis_kremove(redis, m, key, keysz);
}

int redis_set_server(const char *server, int port) {
  if (server == NULL ||
      port < 1) {
    errno = EINVAL;
    return -1;
  }

  redis_server = server;
  redis_port = port;
  return 0;
}

int redis_set_timeouts(unsigned long connect_millis, unsigned long io_millis) {
  redis_connect_millis = connect_millis;
  redis_io_millis = io_millis;

  return 0;
}

int redis_clear(void) {
  if (sess_redis != NULL) {
    pr_redis_conn_destroy(sess_redis);
    sess_redis = NULL;
  }

  return 0;
}

int redis_init(void) {
  return 0;
}

#else

pr_redis_t *pr_redis_conn_get(pool *p) {
  errno = ENOSYS;
  return NULL;
}

pr_redis_t *pr_redis_conn_new(pool *p, module *m, unsigned long flags) {
  errno = ENOSYS;
  return NULL;
}

int pr_redis_conn_close(pr_redis_t *redis) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_conn_destroy(pr_redis_t *redis) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_conn_clone(pool *p, pr_redis_t *redis) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_conn_set_namespace(pr_redis_t *redis, module *m,
    const char *prefix) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_add(pr_redis_t *redis, module *m, const char *key, void *value,
    size_t valuesz, time_t expires) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_decr(pr_redis_t *redis, module *m, const char *key, uint32_t decr,
    uint64_t *value) {
  errno = ENOSYS;
  return -1;
}

void *pr_redis_get(pool *p, pr_redis_t *redis, module *m, const char *key,
    size_t *valuesz) {
  errno = ENOSYS;
  return NULL;
}

char *pr_redis_get_str(pool *p, pr_redis_t *redis, module *m, const char *key) {
  errno = ENOSYS;
  return NULL;
}

int pr_redis_incr(pr_redis_t *redis, module *m, const char *key, uint32_t incr,
    uint64_t *value) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_remove(pr_redis_t *redis, module *m, const char *key) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set(pr_redis_t *redis, module *m, const char *key, void *value,
    size_t valuesz, time_t expires) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_count(pr_redis_t *redis, module *m, const char *key,
    uint64_t *count) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_delete(pr_redis_t *redis, module *m, const char *key,
    const char *field) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_exists(pr_redis_t *redis, module *m, const char *key,
    const char *field) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_get(pool *p, pr_redis_t *redis, module *m, const char *key,
    const char *field, void **value, size_t *valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_getall(pool *p, pr_redis_t *redis, module *m,
    const char *key, pr_table_t **hash) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_incr(pr_redis_t *redis, module *m, const char *key,
    const char *field, int32_t incr, int64_t *value) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_keys(pool *p, pr_redis_t *redis, module *m, const char *key,
    array_header **fields) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_remove(pr_redis_t *redis, module *m, const char *key) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_set(pr_redis_t *redis, module *m, const char *key,
    const char *field, void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_setall(pr_redis_t *redis, module *m, const char *key,
    pr_table_t *hash) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_values(pool *p, pr_redis_t *redis, module *m,
    const char *key, array_header **values) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_append(pr_redis_t *redis, module *m, const char *key,
    void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_count(pr_redis_t *redis, module *m, const char *key,
    uint64_t *count) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_delete(pr_redis_t *redis, module *m, const char *key,
    void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_exists(pr_redis_t *redis, module *m, const char *key,
    unsigned int idx) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_remove(pr_redis_t *redis, module *m, const char *key) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_set(pr_redis_t *redis, module *m, const char *key,
    unsigned int idx, void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set_add(pr_redis_t *redis, module *m, const char *key,
    void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set_count(pr_redis_t *redis, module *m, const char *key,
    uint64_t *count) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set_delete(pr_redis_t *redis, module *m, const char *key,
    void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set_exists(pr_redis_t *redis, module *m, const char *key,
    void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set_remove(pr_redis_t *redis, module *m, const char *key) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_kadd(pr_redis_t *redis, module *m, const char *key, size_t keysz,
    void *value, size_t valuesz, time_t expires) {
  errno = ENOSYS;
  return -1;
}

void *pr_redis_kget(pool *p, pr_redis_t *redis, module *m, const char *key,
    size_t keysz, size_t *valuesz) {
  errno = ENOSYS;
  return NULL;
}

char *pr_redis_kget_str(pool *p, pr_redis_t *redis, module *m, const char *key,
    size_t keysz) {
  errno = ENOSYS;
  return NULL;
}

int pr_redis_kremove(pr_redis_t *redis, module *m, const char *key,
    size_t keysz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_kset(pr_redis_t *redis, module *m, const char *key, size_t keysz,
    void *value, size_t valuesz, time_t expires) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_kcount(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, uint64_t *count) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_kdelete(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, const char *field, size_t fieldsz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_kexists(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, const char *field, size_t fieldsz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_kget(pool *p, pr_redis_t *redis, module *m, const char *key,
    size_t keysz, const char *field, size_t fieldsz, void **value,
    size_t *valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_kgetall(pool *p, pr_redis_t *redis, module *m,
    const char *key, size_t keysz, pr_table_t **hash) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_kincr(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, const char *field, size_t fieldsz, int32_t incr,
    int64_t *value) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_kkeys(pool *p, pr_redis_t *redis, module *m, const char *key,
    size_t keysz, array_header **fields) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_kremove(pr_redis_t *redis, module *m, const char *key,
    size_t keysz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_kset(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, const char *field, size_t fieldsz, void *value,
    size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_ksetall(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, pr_table_t *hash) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_hash_kvalues(pool *p, pr_redis_t *redis, module *m,
  const char *key, size_t keysz, array_header **values);

int pr_redis_list_kappend(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_kcount(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, uint64_t *count) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_kdelete(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_kexists(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, unsigned int idx) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_kremove(pr_redis_t *redis, module *m, const char *key,
    size_t keysz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_list_kset(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, unsigned int idx, void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set_kadd(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set_kcount(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, uint64_t *count) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set_kdelete(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set_kexists(pr_redis_t *redis, module *m, const char *key,
    size_t keysz, void *value, size_t valuesz) {
  errno = ENOSYS;
  return -1;
}

int pr_redis_set_kremove(pr_redis_t *redis, module *m, const char *key,
    size_t keysz) {
  errno = ENOSYS;
  return -1;
}

int redis_set_server(const char *server, int port) {
  errno = ENOSYS;
  return -1;
}

int redis_set_timeouts(unsigned long conn_millis, unsigned long io_millis) {
  errno = ENOSYS;
  return -1;
}

int redis_clear(void) {
  errno = ENOSYS;
  return -1;
}

int redis_init(void) {
  errno = ENOSYS;
  return -1;
}

#endif /* PR_USE_REDIS */