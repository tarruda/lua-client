#define LUA_LIB
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
# include <sys/wait.h>
#endif

#include <lua.h>
#include <lauxlib.h>
#include <uv.h>

#define UNUSED(x) ((void)x)

#define LOOP_META_NAME "nvim.Loop"
#define STREAM_META_NAME "nvim.Stream"

typedef enum {
  StreamTypeStdio,
  StreamTypeChild
} StreamType;

typedef struct {
  uv_loop_t loop;
  uv_prepare_t prepare;
  uv_timer_t timer;
  lua_State *L;
  bool running;
  int refcount;
  uint32_t timeout;
} UVLoop;

typedef struct {
  UVLoop *loop;
  char read_buffer[0xffff];
  uv_stream_t *sink, *source;
  StreamType type;
  int refcount, lref, read_cb;
  bool reading, active, closed;
  union {
    struct {
      uv_process_t process;
      uv_process_options_t process_options;
      uv_stdio_container_t stdio[3];
      uv_pipe_t in, out;
      bool exited;
    } child;
    struct {
      uv_pipe_t in, out;
    } std;
  } data;
} UVStream;

static int stream_close(lua_State *L);

static void *newudata(lua_State *L, size_t size)
{
  void *rv = malloc(size);
  if (!rv) luaL_error(L, "Failed to allocate memory");
  *(void **)lua_newuserdata(L, sizeof(void *)) = rv;
  return rv;
}

static UVLoop *checkloop(lua_State *L)
{
  return *(UVLoop **)luaL_checkudata(L, 1, LOOP_META_NAME);
}

static UVStream *checkstream(lua_State *L)
{
  return *(UVStream **)luaL_checkudata(L, 1, STREAM_META_NAME);
}

static void walk_cb(uv_handle_t *handle, void *arg)
{
  UNUSED(arg);

  if (!uv_is_closing(handle)) {
    uv_close(handle, NULL);
  }
}

static void loop_close(UVLoop *loop)
{
  uv_walk(&loop->loop, walk_cb, NULL);
  /* If all handles are not closed, this will be an infinite loop */
  while (uv_loop_close(&loop->loop)) {
    uv_run(&loop->loop, UV_RUN_DEFAULT);
  }
}

static void loop_decref(UVLoop *loop)
{
  assert(loop->refcount);
  if (--loop->refcount) return;
  loop_close(loop);
  free(loop);
}

static void stream_decref(UVStream *stream)
{
  assert(stream->refcount);
  if (--stream->refcount) return;
  loop_decref(stream->loop);
  free(stream);
}

static void timer_cb(uv_timer_t *handle)
{
  UVLoop *loop = handle->data;
  uv_stop(&loop->loop);
}

static void prepare_cb(uv_prepare_t *handle)
{
  UVLoop *loop = handle->data;
  uv_timer_start(&loop->timer, timer_cb, loop->timeout, 0);
  uv_prepare_stop(handle);
}

static void stream_close_cb(uv_handle_t *handle)
{
  stream_decref(handle->data);
}

static void exit_cb(uv_process_t *proc, int64_t status, int term_signal)
{
  UVStream *stream = proc->data;
  UNUSED(status);
  UNUSED(term_signal);

  stream->data.child.exited = true;
  /* uv_stop will be called by read_cb once all data is consumed */
}

static void alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
  UVStream *stream = handle->data;
  UNUSED(suggested);

  if (stream->reading) {
    buf->len = 0;
    buf->base = NULL;
    return;
  }

  buf->len = sizeof(stream->read_buffer);
  buf->base = stream->read_buffer;
  stream->reading = true;
}

static void read_cb(uv_stream_t *s, ssize_t cnt, const uv_buf_t *buf)
{
  UVStream *stream = s->data;
  UNUSED(buf);

  if (cnt <= 0) {
    if (cnt != UV_ENOBUFS) {
      /* FIXME stream->loop->last_error = uv_strerror((int)cnt); */
      uv_read_stop(s);
      uv_stop(&stream->loop->loop);
    }
    return;
  }

  /* push data_cb */
  lua_rawgeti(stream->loop->L, LUA_REGISTRYINDEX, stream->read_cb);
  /* push read buffer */
  lua_pushlstring(stream->loop->L, stream->read_buffer, (size_t)cnt);
  /* call data_cb */
  lua_call(stream->loop->L, 1, 0);
  /* restore reading state */
  stream->reading = false;
}

static void write_cb(uv_write_t *req, int status)
{
  UNUSED(status);

  free(req->data);
  free(req);
}

static int loop_new(lua_State *L)
{
  UVLoop *loop = newudata(L, sizeof(UVLoop));
  loop->L = L;
  loop->timeout = 0;
  loop->refcount = 1;
  loop->running = false;
  uv_loop_init(&loop->loop);
  uv_prepare_init(&loop->loop, &loop->prepare);
  uv_timer_init(&loop->loop, &loop->timer);
  loop->prepare.data = loop;
  loop->timer.data = loop;
  luaL_getmetatable(L, LOOP_META_NAME);
  lua_setmetatable(L, -2);
  return 1;
}

static int loop_stdio(lua_State *L)
{
  int status;
  const char *error = NULL;
  UVLoop *loop = checkloop(L);
  UVStream *stream = newudata(L, sizeof(UVStream));

  stream->loop = loop;
  stream->type = StreamTypeStdio;
  stream->closed = false;
  stream->reading = false;
  stream->active = false;
  
  status = uv_pipe_init(&loop->loop, &stream->data.std.in, 0);
  assert(status == 0);
  stream->data.std.in.data = stream;
  status = uv_pipe_init(&loop->loop, &stream->data.std.out, 0);
  assert(status == 0);
  stream->data.std.out.data = stream;

  stream->sink = (uv_stream_t *)&stream->data.std.out;
  stream->source = (uv_stream_t *)&stream->data.std.in;

  loop->refcount++;
  stream->refcount = 2;

  if ((status = uv_pipe_open(&stream->data.std.in, 0))
   || (status = uv_pipe_open(&stream->data.std.out, 1))) {
    stream_close(L);
    error = uv_strerror(status);
    goto end;
  }

end:
  if (error) {
    luaL_error(L, error);
  }
  stream->refcount++;
  luaL_getmetatable(L, STREAM_META_NAME);
  lua_setmetatable(L, -2);
  return 1;
}

static int loop_spawn(lua_State *L)
{
  int status;
  size_t i, len;
  char **argv = NULL;
  const char *error = NULL;
  uv_process_t *proc;
  uv_process_options_t *opts;
  UVStream *stream = NULL;
  UVLoop *loop = checkloop(L);

  luaL_checktype(L, 2, LUA_TTABLE);
  len = lua_objlen(L, -1);  /* get size of table */
  if (!len) {
    error = "`spawn` argv must have at least one string";
    goto end;
  }

  argv = calloc(len + 1, sizeof(char *));
  if (!argv) luaL_error(L, "Failed to allocate memory");

  for (i = 1; i <= len; i++) {
    lua_pushinteger(L, (int)i);
    lua_gettable(L, -2);

    if (lua_type(L, -1) != LUA_TSTRING) {
      error = "`spawn` argv has non-string entries";
      goto end;
    }

    argv[i - 1] = (char *)lua_tostring(L, -1);
    lua_pop(L, 1);
  }

  stream = newudata(L, sizeof(UVStream));
  stream->loop = loop;
  stream->type = StreamTypeChild;
  stream->closed = false;
  stream->reading = false;
  stream->active = false;
  stream->data.child.exited = false;

  status = uv_pipe_init(&stream->loop->loop, &stream->data.child.in, 0);
  assert(status == 0);
  stream->data.child.in.data = stream;
  status = uv_pipe_init(&stream->loop->loop, &stream->data.child.out, 0);
  assert(status == 0);
  stream->data.child.out.data = stream;

  stream->sink = (uv_stream_t *)&stream->data.child.in;
  stream->source = (uv_stream_t *)&stream->data.child.out;

  loop->refcount++;
  stream->refcount = 2;

  proc = &stream->data.child.process;
  opts = &stream->data.child.process_options;

  proc->data = stream;
  opts->file = argv[0];
  opts->args = argv;
  opts->stdio = stream->data.child.stdio;
  opts->stdio_count = 3;
  opts->flags = UV_PROCESS_WINDOWS_HIDE;
  opts->exit_cb = exit_cb;
  opts->cwd = NULL;
  opts->env = NULL;

  stream->data.child.stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
  stream->data.child.stdio[0].data.stream =
    (uv_stream_t *)&stream->data.child.in;

  stream->data.child.stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  stream->data.child.stdio[1].data.stream =
    (uv_stream_t *)&stream->data.child.out;

  stream->data.child.stdio[2].flags = UV_IGNORE;
  stream->data.child.stdio[2].data.fd = 2;

  /* Spawn the process */
  if ((status = uv_spawn(&loop->loop, proc, opts))) {
    stream->data.child.exited = true;
    stream_close(L);
    error = uv_strerror(status);
    goto end;
  }

end:
  free(argv);
  if (error) {
    luaL_error(L, error);
  }

  stream->refcount += 2;
  luaL_getmetatable(L, STREAM_META_NAME);
  lua_setmetatable(L, -2);
  return 1;
}

static int loop_run(lua_State *L)
{
  UVLoop *loop = checkloop(L);
  uv_run_mode mode = UV_RUN_DEFAULT;

  if (loop->running) {
    luaL_error(L, "Loop already running");
  }

  loop->timeout = 0;

  if (!lua_isnoneornil(L, 2)) {
    int timeout = luaL_checkint(L, 2);
    if (timeout < 0) {
      luaL_error(L, "Timeout argument must be a positive integer");
    } else if (timeout == 0) {
      mode = UV_RUN_NOWAIT;
    } else {
      loop->timeout = (uint32_t)timeout;
      uv_prepare_start(&loop->prepare, prepare_cb);
    }
    lua_pop(L, 1);
  }

  loop->running = true;
  uv_run(&loop->loop, mode);
  loop->running = false;

  if (mode == UV_RUN_DEFAULT) {
    uv_prepare_stop(&loop->prepare);
    uv_timer_stop(&loop->timer);
  }

  return 0;
}

static int loop_stop(lua_State *L) {
  UVLoop *loop = checkloop(L);
  uv_stop(&loop->loop);
  return 0;
}

static int loop_delete(lua_State *L)
{
  UVLoop *loop = checkloop(L);
  loop_decref(loop);
  return 0;
}

static int stream_read_start(lua_State *L)
{
  UVStream *stream = checkstream(L);
  /* Store the data callback on the registry and save the reference */
  stream->read_cb = luaL_ref(L, LUA_REGISTRYINDEX);
  uv_read_start(stream->source, alloc_cb, read_cb);
  stream->active = true;
  return 0;
}

static int stream_read_stop(lua_State *L)
{
  UVStream *stream = checkstream(L);
  uv_read_stop(stream->source);
  luaL_unref(L, LUA_REGISTRYINDEX, stream->read_cb);
  stream->active = false;
  return 0;
}

static int stream_write(lua_State *L)
{
  uv_buf_t buf;
  uv_write_t *req;
  const char *data;
  int status;
  UVStream *stream = checkstream(L);
 
  data = luaL_checklstring(L, 2, &buf.len);
  req = malloc(sizeof(uv_write_t));
  req->data = buf.base = memcpy(malloc(buf.len), data, buf.len);
  status = uv_write(req, stream->sink, &buf, 1, write_cb);

  if (status) {
    /* stream->loop->last_error = uv_strerror(status); */
    free(buf.base);
    free(req);
    /* luaL_error(L, stream->loop->last_error); */
  }

  return 0;
}

static int stream_close(lua_State *L)
{
  UVStream *stream = checkstream(L);

  if (stream->closed) {
    return 0;
  }

  stream->closed = true;

  if (stream->active) {
    uv_read_stop(stream->source);
    stream->active = false;
  }

  if (stream->type == StreamTypeChild && !lua_isnone(L, 2) && lua_isnumber(L, 2)
      && !stream->data.child.exited) {
    int sigkill = luaL_checkint(L, 2);
    kill(stream->data.child.process.pid, sigkill ? SIGKILL : SIGTERM);
  }

  uv_close((uv_handle_t *)stream->sink, stream_close_cb);
  uv_close((uv_handle_t *)stream->source, stream_close_cb);
  if (stream->type == StreamTypeChild) {
    uv_close((uv_handle_t *)&stream->data.child.process, stream_close_cb);
  }
  /* spin the loop to remove all references to the stream memory, but first add
   * a reference to stop it from being freed since we still use it below */
  stream->refcount++;
  uv_run(&stream->loop->loop, UV_RUN_NOWAIT);
  if (stream->refcount == 1) {
    stream_decref(stream);
    return 0;
  }
  stream_decref(stream);

#ifndef _WIN32
  if (stream->type == StreamTypeChild) {
    if (!stream->data.child.exited) {
      /* Work around libuv bug that leaves defunct children:
       * https://github.com/libuv/libuv/issues/154 */
      while (!kill(stream->data.child.process.pid, 0)) {
        waitpid(stream->data.child.process.pid, NULL, WNOHANG);
      }
    }
  }
#endif

  return 0;
}

static int stream_delete(lua_State *L)
{
  UVStream *stream = checkstream(L);
  (void)stream_close(L);
  stream_decref(stream);
  return 0;
}

static int tb_panic(lua_State *L)
{
  /* same as the default panic function, but also print the lua traceback */
  const char *s = lua_tostring(L, -1);
  fputs("PANIC: unprotected error in call to Lua API (", stderr);
  fputs(s ? s : "?", stderr);
  fputc(')', stderr); fputc('\n', stderr);
  lua_getfield(L, LUA_GLOBALSINDEX, "debug");
  lua_getfield(L, -1, "traceback");
  lua_pushvalue(L, 1);
  lua_pushinteger(L, 2);
  lua_call(L, 2, 1);
  fputs(lua_tostring(L, -1), stderr);
  fflush(stderr);
  return 0;
}

static const luaL_reg module_functions[] = {
  {"Loop", loop_new},
  {NULL, NULL}
};

static const luaL_reg loop_methods[] = {
  {"stdio", loop_stdio},
  {"spawn", loop_spawn},
  {"run", loop_run},
  {"stop", loop_stop},
  {"__gc", loop_delete},
  {NULL, NULL}
};

static const luaL_reg stream_methods[] = {
  {"read_start", stream_read_start},
  {"read_stop", stream_read_stop},
  {"write", stream_write},
  {"close", stream_close},
  {"__gc", stream_delete},
  {NULL, NULL}
};

int luaopen_nvim_uv(lua_State *L) {
  /* register UVLoop metatable */
  luaL_newmetatable(L, LOOP_META_NAME);
  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_settable(L, -3);
  luaL_register(L, NULL, loop_methods);
  /* register UVStream metatable */
  luaL_newmetatable(L, STREAM_META_NAME);
  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_settable(L, -3);
  luaL_register(L, NULL, stream_methods);
  lua_atpanic(L, tb_panic);
  /* create and return module */
  lua_newtable(L);
  luaL_register(L, NULL, module_functions);
  return 1;
}
