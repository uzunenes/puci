-- puci.nvim plugin entry: registers :Puci and :PuciOpen commands
if vim.g.loaded_puci then return end
vim.g.loaded_puci = 1

vim.api.nvim_create_user_command("Puci", function(opts)
  local puci = require("puci")
  puci.open({model = opts.args ~= "" and opts.args or nil})
end, {nargs="?", desc="Open PUCI chat (optional model arg)"})

vim.api.nvim_create_user_command("PuciOpen", function(opts)
  require("puci").open({model = opts.args ~= "" and opts.args or nil})
end, {nargs="?", desc="Open PUCI chat"})

vim.api.nvim_create_user_command("PuciCancel", function()
  -- cancels via global session if present (user can store session handle)
  vim.notify("[puci] PuciCancel: call session:cancel() on your session handle", vim.log.levels.INFO)
end, {desc="Cancel PUCI turn (use session:cancel())"})
