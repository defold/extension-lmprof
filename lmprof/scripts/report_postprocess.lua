local M = {}

function M.filter_out_lines_wit(filename_in, filename_out, filter_out)
	local f_in = io.open(filename_in, "r")
	local f_out = io.open(filename_out, "w")
	for line in f_in:lines() do
		if line:find(filter_out) == nil then
			f_out:write(line, "\n")
		end
	end
	f_in:close()
	f_out:close()
end

return M
