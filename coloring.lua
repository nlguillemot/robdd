local function color(connections, names)
    local edges = {}
    local coloring = true
    for prefecture,neighbors in ipairs(connections) do
        if next(neighbors) == nil then
            local island0 = input[names[prefecture] .. '[0]']
            local island1 = input[names[prefecture] .. '[1]']
        else
            for _,neighbor in ipairs(neighbors) do
                local edgekey1, edgekey2 = prefecture, neighbor
                if edgekey2 < edgekey1 then
                    edgekey1,edgekey2 = edgekey2,edgekey1
                end
                local edgekey = tostring(edgekey1) .. ',' .. tostring(edgekey2)
                if not edges[edgekey] then
                    edges[edgekey] = true
                    local ai0 = input[names[edgekey1] .. '[0]']
                    local ai1 = input[names[edgekey1] .. '[1]']
                    local aj0 = input[names[edgekey2] .. '[0]']
                    local aj1 = input[names[edgekey2] .. '[1]']
                    coloring = coloring * ((ai0 ^ aj0) + (ai1 ^ aj1))
                end
            end
        end
    end
    return coloring
end

return {
    color = color
}