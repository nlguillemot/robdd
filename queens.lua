n = 4

title = tostring(n) .. '-queens problem'

board = true
for row=n,1,-1 do
    for col=n,1,-1 do
       -- OR all the variables on the same row/col/diagonal into T
       -- in order to enforce that only one queen is on each row/col/diagonal
       T = false
       -- cannot overlap with other queens on the same row
       -- (optimization: only consider cells to their right)
       for col2=col+1,n do
           T = T + input['r' .. tostring(row) .. 'c' .. tostring(col2)]
       end
       -- cannot overlap with other queens in the same column
       -- (optimization: only consider cells above them)
       for row2=row+1,n do
           T = T + input['r' .. tostring(row2) .. 'c' .. tostring(col)]
       end
       -- cannot overlap with other queens in the same diagonal
       -- (optimization: only consider cells top-right and top-left of them)
       -- top left:
       for col2=col-1,1,-1 do
           if row+(col-col2) > n then
               break
           end
           T = T + input['r' .. tostring(row+(col-col2)) .. 'c' .. tostring(col2)]
       end
       -- top right:
       for col2=col+1,n do
           if row+(col2-col) > n then
               break
           end
           T = T + input['r' .. tostring(row+(col2-col)) .. 'c' .. tostring(col2)]
       end
       -- output['T' .. 'r' .. tostring(row) .. 'c' .. tostring(col)] = T
       -- output['constraint ' .. 'r' .. tostring(row) .. 'c' .. tostring(col)] = -(T * input['r' .. tostring(row) .. 'c' .. tostring(col)])
       board = board * -(T * input['r' .. tostring(row) .. 'c' .. tostring(col)])
    end
    -- OR the row into T, since each row must have at least one queen
    T = false
    for col=n,1,-1 do
        T = T + input['r' .. tostring(row) .. 'c' .. tostring(col)]
    end
    -- output['row ' .. tostring(row)] = T
    board = board * T
end

output.board = board