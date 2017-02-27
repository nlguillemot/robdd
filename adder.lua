n = 2
display = true

title = tostring(n) .. '-bit ripple carry adder'

-- declare the inputs backwards to allow the circuit to reuse previous nodes
for i=n,1,-1 do
    _ = input['a' .. tostring(i - 1)]
    _ = input['b' .. tostring(i - 1)]
end

cin = input.cin

for i=1,n do
    a = input['a' .. tostring(i - 1)]
    b = input['b' .. tostring(i - 1)]
    
    cout = a * b + cin * (a ^ b)
    output['s' .. tostring(i - 1)] = a ^ b ^ cin
    
    cin = cout
end

output.cout = cin
