
-- File: config.lua
s2e = {
  kleeArgs = {
    -- Run each state for at least 1 second before
    -- switching to the other:
    "--use-batching-search=true", "--batch-time=1.0"
  }
}
plugins = {
  -- Enable a plugin that handles S2E custom opcode
  "BaseInstructions",
  "ExecutionStaticCollector"
}
pluginsConfig = {
}
pluginsConfig.ExecutionStaticCollector = {
}
