local M = {}

function M.filter_out_lines_with(filename_in, filename_out, filter_out)
    local f_in = io.open(filename_in, "r")
    local f_out = io.open(filename_out, "w")
    local write
    local lines_read = 0
    local lines_write = 0
    for line in f_in:lines() do
        write = true
        lines_read =  lines_read+ 1
        for _, v in pairs(filter_out) do
            if line:find(v) ~= nil then
                write = false
            end
        end
        if write then
            lines_write=lines_write +1
            f_out:write(line, "\n")
        end
    end
    print("Removed lines:", ((1 - lines_write/lines_read)*100).."%")
    f_in:close()
    f_out:close()
end

return M
