local uv = require('luv')


local ChildProcessStream = {}
ChildProcessStream.__index = ChildProcessStream

function ChildProcessStream.spawn(argv)
  local self = setmetatable({
    _child_stdin = uv.new_pipe(false),
    _child_stdout = uv.new_pipe(false)
  }, ChildProcessStream)
  local prog = argv[1]
  local args = {}
  for i = 2, #argv do
    args[#args + 1] = argv[i]
  end
  self._proc, self._pid = uv.spawn(prog, {
    stdio = {self._child_stdin, self._child_stdout, 2},
    args = args,
  }, function()
    self:close()
  end)
  return self
end

function ChildProcessStream:write(data)
  self._child_stdin:write(data)
end

function ChildProcessStream:read_start(cb)
  self._child_stdout:read_start(function(err, chunk)
    if err then
      error(err)
    end
    cb(chunk)
  end)
end

function ChildProcessStream:read_stop()
  self._child_stdout:read_stop()
end

function ChildProcessStream:close(kill)
  if self._closed then
    return
  end
  self._closed = true
  self:read_stop()
  self._child_stdin:close()
  self._child_stdout:close()
  if kill then
    self._proc:kill('sigkill')
  else
    self._proc:kill()
  end
end

return ChildProcessStream
