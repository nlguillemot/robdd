n = 2

title = tostring(n) .. '-bit ripple carry adder'

outputs = {}

for i=n,1,-1 do
    a = var['a' .. tostring(i - 1)]
    b = var['b' .. tostring(i - 1)]
end

cin = var.cin

for i=1,n do
    a = var['a' .. tostring(i - 1)]
    b = var['b' .. tostring(i - 1)]
    
    cout =  a * b + cin * (a ^ b)
    outputs['s' .. tostring(i - 1)] = a ^ b ^ cin
    
    cin = cout
end

outputs.cout = cin

return outputs
