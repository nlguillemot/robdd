local a = var.a
local b = var.b
local c = var.c
local r1 = a*b + a*c + b*c
local r2 = b*c

return {r1 = r1, r2 = r2}
