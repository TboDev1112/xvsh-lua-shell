xvsh.alias("nano", "nvim")
xvsh.alias("vim", "nvim")
xvsh.alias("vi", "nvim")
xvsh.alias("micro", "nvim")

xvsh.prompt = function()
    local home = xvsh.getenv("HOME") or ""
    local cwd  = xvsh.cwd():gsub("^" .. home, "~")

    return string.format(
        "\27[2m%s\27[0m\n\27[32mxvsh>\27[0m ",
        cwd
    )
end


