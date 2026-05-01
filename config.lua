--
-- examples/config.lua — example Xvsh configuration
--
-- Place the absolute path to this file in ~/.xvshrc:
--   /home/you/xvsh-scripts/config.lua
--

-- ── ANSI colour helpers ────────────────────────────────────────────
local function fg(code, s) return string.format("\27[%dm%s\27[0m", code, s) end
local red     = function(s) return fg(31, s) end
local green   = function(s) return fg(32, s) end
local yellow  = function(s) return fg(33, s) end
local blue    = function(s) return fg(34, s) end
local cyan    = function(s) return fg(36, s) end
local bold    = function(s) return string.format("\27[1m%s\27[0m", s) end

-- ── Dynamic prompt ────────────────────────────────────────────────
--
-- xvsh.prompt can be:
--   • a plain string:  xvsh.prompt = "$ "
--   • a function:      xvsh.prompt = function() return "..." end
--
-- The function is called before every line is read so you can include
-- the current directory, git branch, exit code, etc.
--
xvsh.prompt = function()
    local user   = xvsh.getenv("USER") or "user"
    local cwd    = xvsh.cwd()
    local home   = xvsh.getenv("HOME") or ""
    local code   = xvsh.exit_code()

    -- Replace $HOME prefix with ~
    if cwd:sub(1, #home) == home then
        cwd = "~" .. cwd:sub(#home + 1)
    end

    -- Try to get git branch (using command substitution via os.execute + tmp file)
    local branch = ""
    local tmp = os.tmpname()
    if os.execute("git rev-parse --abbrev-ref HEAD > " .. tmp .. " 2>/dev/null") then
        local f = io.open(tmp, "r")
        if f then
            branch = f:read("*l") or ""
            f:close()
        end
    end
    os.remove(tmp)

    local exit_indicator = (code ~= 0)
        and red(" [" .. code .. "]")
        or  ""

    local git_part = (branch ~= "") and (" " .. cyan("(" .. branch .. ")")) or ""

    return string.format("%s %s%s%s\n%s ",
        bold(green(user)),
        blue(cwd),
        git_part,
        exit_indicator,
        (code ~= 0) and red("❯") or green("❯")
    )
end

-- ── Aliases ───────────────────────────────────────────────────────
xvsh.alias("ll",    "ls -lah --color=auto")
xvsh.alias("la",    "ls -A --color=auto")
xvsh.alias("l",     "ls --color=auto")
xvsh.alias("...",   "cd ../..")
xvsh.alias("..",    "cd ..")
xvsh.alias("gs",    "git status")
xvsh.alias("ga",    "git add")
xvsh.alias("gc",    "git commit")
xvsh.alias("gp",    "git push")
xvsh.alias("gl",    "git log --oneline --graph --decorate")
xvsh.alias("vi",    "vim")
xvsh.alias("py",    "python3")
xvsh.alias("serve", "python3 -m http.server")

-- ── Environment variables ─────────────────────────────────────────
xvsh.setenv("EDITOR",    "vim")
xvsh.setenv("PAGER",     "less")
xvsh.setenv("LESS",      "-R")   -- enable colour output in less

-- Extend PATH (only if not already present)
local path = xvsh.getenv("PATH") or ""
local extra_paths = {
    xvsh.getenv("HOME") .. "/.local/bin",
    xvsh.getenv("HOME") .. "/bin",
}
for _, p in ipairs(extra_paths) do
    if not path:find(p, 1, true) then
        xvsh.setenv("PATH", p .. ":" .. path)
        path = p .. ":" .. path
    end
end

-- ── Hooks ─────────────────────────────────────────────────────────

-- Called BEFORE each command is executed.
-- Receives the raw command string as typed.
xvsh.on_preexec(function(cmd)
    -- Example: print a subtle execution indicator
    -- (Remove or adjust to your taste)
    io.write(string.format("\27[90m▸ %s\27[0m\n", cmd))
end)

-- Called AFTER each command finishes.
-- Receives the command string and integer exit code.
xvsh.on_postexec(function(cmd, code)
    -- Example: ring the bell if a long-running command fails
    -- (We don't know duration here, but you could track it)
    if code ~= 0 then
        io.stderr:write(string.format("\27[31m✗ exited %d\27[0m\n", code))
    end
end)

-- ── Lua-defined built-in commands ─────────────────────────────────
--
-- Use xvsh.register_builtin(name, fn) to add commands implemented
-- entirely in Lua.  The function receives a table `args` where
-- args[0] is the command name and args[1..n] are the arguments.
-- Return an integer exit code (0 = success).
--

-- `hello [name]` — a trivial example builtin
xvsh.register_builtin("hello", function(args)
    local name = args[1] or xvsh.getenv("USER") or "world"
    print(string.format("Hello, %s! (from Lua)", name))
    return 0
end)

-- `mkcd <dir>` — create a directory and cd into it
xvsh.register_builtin("mkcd", function(args)
    local dir = args[1]
    if not dir then
        io.stderr:write("mkcd: missing directory argument\n")
        return 1
    end
    -- os.execute returns true on success in Lua 5.4
    if not os.execute("mkdir -p " .. dir) then
        io.stderr:write("mkcd: failed to create " .. dir .. "\n")
        return 1
    end
    -- We cannot actually change directory from a child process,
    -- so use xvsh's built-in cd mechanism by modifying the environment.
    -- The cleaner approach: write a tiny wrapper in your shell script.
    io.write("Created " .. dir .. "\n")
    return 0
end)

-- `reload` — re-source the config files listed in ~/.xvshrc
xvsh.register_builtin("reload", function(_args)
    local home = xvsh.getenv("HOME")
    if not home then
        io.stderr:write("reload: HOME not set\n")
        return 1
    end
    local rc = io.open(home .. "/.xvshrc", "r")
    if not rc then
        io.stderr:write("reload: ~/.xvshrc not found\n")
        return 1
    end
    for line in rc:lines() do
        line = line:match("^%s*(.-)%s*$") -- trim
        if line ~= "" and line:sub(1,1) ~= "#" then
            local ok, err = loadfile(line)
            if ok then
                local success, msg = pcall(ok)
                if not success then
                    io.stderr:write("reload: error in " .. line .. ": " .. msg .. "\n")
                end
            else
                io.stderr:write("reload: " .. (err or "unknown error") .. "\n")
            end
        end
    end
    rc:close()
    print("Config reloaded.")
    return 0
end)
