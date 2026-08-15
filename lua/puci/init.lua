-- puci.nvim - Neovim integration for libpuci
-- Zero-dependency Lua plugin. Requires puci.so built via `meson setup build -Dlua=true`
-- or `cc -fPIC -shared bindings/lua/puci_lua.c -o puci.so $(pkg-config --cflags --libs puci)`
-- (omit -llua when loaded inside Neovim which already provides Lua symbols).

local M = {}

local puci = nil
local function ensure_puci()
  if puci then return puci end
  local ok, mod = pcall(require, "puci")
  if not ok then
    vim.notify("[puci] puci.so not found. Build with: meson setup build -Dlua=true && ninja -C build", vim.log.levels.ERROR)
    return nil
  end
  puci = mod
  return puci
end

-- Session wrapper
local Session = {}
Session.__index = Session

function Session:send(prompt, opts)
  opts = opts or {}
  local on_delta = opts.on_delta
  local on_tool = opts.on_tool
  local on_done = opts.on_done
  local on_error = opts.on_error
  local sess = self._sess
  if not sess then return nil, "no session" end
  local st = sess:begin_turn(prompt)
  if st ~= 0 then return nil, "begin_turn failed: " .. tostring(st) end
  -- poll loop driven via vim.loop timer (non-blocking, integrates with Neovim event loop)
  local timer = vim.loop.new_timer()
  local buf = ""
  local function step()
    local pst = sess:process()
    local ev = sess:next_event()
    while ev do
      if ev.kind == 0 and ev.text and on_delta then -- TOKEN_DELTA
        on_delta(ev.text)
        buf = buf .. ev.text
      elseif ev.kind == 1 and on_tool then
        on_tool(ev)
      elseif ev.kind == 3 then -- TURN_COMPLETE
        timer:stop(); timer:close()
        if on_done then on_done(buf, ev) end
        return
      elseif ev.kind == 4 then -- ERROR
        timer:stop(); timer:close()
        if on_error then on_error(ev) end
        return
      elseif ev.kind == 6 then -- TURN_CANCELLED
        timer:stop(); timer:close()
        if on_error then on_error(ev) end
        return
      end
      ev = sess:next_event()
    end
    if pst ~= 0 then
      timer:stop(); timer:close()
      if pst ~= 0 and on_error then on_error({kind=pst}) end
    end
  end
  timer:start(10, 10, vim.schedule_wrap(step))
  return true
end

function Session:poll_step()
  if not self._sess then return nil end
  local sess = self._sess
  sess:process()
  local events = {}
  local ev = sess:next_event()
  while ev do
    table.insert(events, ev)
    ev = sess:next_event()
  end
  return events
end

function Session:cancel()
  if self._sess then return self._sess:cancel() end
end

function Session:register_tool(name, desc, fn)
  if not self._sess then return nil end
  return self._sess:register_tool(name, desc, fn)
end

function Session:destroy()
  if self._sess then
    -- GC will handle via __gc
    self._sess = nil
  end
end

-- diff preview helper using vim.diff
function M.diff_preview(old_text, new_text)
  local old_lines = vim.split(old_text or "", "\n")
  local new_lines = vim.split(new_text or "", "\n")
  -- vim.diff returns unified diff hunks; fallback to simple display
  local ok, diff = pcall(vim.diff, table.concat(old_lines, "\n"), table.concat(new_lines, "\n"), {result_type="unified"})
  if ok and diff and diff ~= "" then return diff end
  return "(no diff)"
end

function M.create_session(opts)
  opts = opts or {}
  local mod = ensure_puci()
  if not mod then return nil, "puci.so missing" end
  local base = opts.base_url or vim.env.PUCI_BASE_URL or "https://api.openai.com/v1"
  local model = opts.model or vim.env.PUCI_MODEL or "gpt-4o-mini"
  local key = opts.api_key or vim.env.PUCI_API_KEY or ""
  local ctx = mod.context()
  if not ctx then return nil, "context create failed" end
  local sess = ctx:session(base, model, key)
  if not sess then return nil, "session create failed" end
  local s = setmetatable({_ctx=ctx, _sess=sess, _opts=opts}, Session)
  -- buffer read/write helpers
  function s:read_buffer(bufnr)
    bufnr = bufnr or 0
    local lines = vim.api.nvim_buf_get_lines(bufnr, 0, -1, false)
    return table.concat(lines, "\n")
  end
  function s:write_buffer(bufnr, text, preview)
    bufnr = bufnr or 0
    local old = s:read_buffer(bufnr)
    if preview then
      local d = M.diff_preview(old, text)
      local choice = vim.fn.confirm("Apply diff?\n" .. d:sub(1, 800), "&Yes\n&No", 2)
      if choice ~= 1 then return false end
    end
    local lines = vim.split(text, "\n")
    vim.api.nvim_buf_set_lines(bufnr, 0, -1, false, lines)
    return true
  end
  return s
end

-- convenience: open a puci chat buffer
function M.open(opts)
  opts = opts or {}
  vim.cmd("vnew")
  local bufnr = vim.api.nvim_get_current_buf()
  vim.bo[bufnr].filetype = "markdown"
  local sess, err = M.create_session(opts)
  if not sess then vim.notify("[puci] " .. tostring(err), vim.log.levels.ERROR); return end
  vim.api.nvim_buf_set_lines(bufnr, 0, -1, false, {"# PUCI Chat", "", "> Type prompt and press <CR> on prompt line"})
  -- simple mapping: <CR> sends current line as prompt
  vim.keymap.set("n", "<CR>", function()
    local line = vim.api.nvim_get_current_line()
    if line:match("^%s*$") or line:match("^#") then return end
    vim.api.nvim_buf_set_lines(bufnr, -1, -1, false, {"", "> " .. line, ""})
    sess:send(line, {
      on_delta = function(t) vim.schedule(function()
        local lc = vim.api.nvim_buf_line_count(bufnr)
        local last = vim.api.nvim_buf_get_lines(bufnr, lc-1, lc, false)[1] or ""
        vim.api.nvim_buf_set_lines(bufnr, lc-1, lc, false, {last .. t})
      end) end,
      on_done = function() vim.schedule(function()
        vim.api.nvim_buf_set_lines(bufnr, -1, -1, false, {"", "--- done ---", ""})
      end) end,
      on_error = function(e) vim.notify("[puci] error " .. vim.inspect(e), vim.log.levels.ERROR) end,
    })
  end, {buffer=bufnr})
  vim.keymap.set("n", "q", "<cmd>close<CR>", {buffer=bufnr})
  return sess
end

return M
