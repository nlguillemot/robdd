local function color (connections, num_colors, names)
    if num_colors ~= 3 and num_colors ~= 4 then
        error('only num_colors of 3 and 4 supported (my laziness)')
    end
    local edges = {}
    local coloring = true
    local function vertexname (v, b)
        return (names and names[v] or tostring(v)) .. '[' .. tostring(b) .. ']'
    end
    local num_color_bits = math.ceil(math.log(num_colors) / math.log(2))
    for vertex,neighbors in ipairs(connections) do
        if next(neighbors) == nil then
            for c = 1, num_color_bits do
                local island = input[vertexname(vertex, c - 1)]
            end
        else
            for _,neighbor in ipairs(neighbors) do
                local edgekey1, edgekey2 = vertex, neighbor
                if edgekey2 < edgekey1 then
                    edgekey1,edgekey2 = edgekey2,edgekey1
                end
                local edgekey = tostring(edgekey1) .. ',' .. tostring(edgekey2)
                if not edges[edgekey] then
                    edges[edgekey] = true

                    local diff_colors = false
                    for c = 1, num_color_bits do
                        local a = input[vertexname(edgekey1, c - 1)]
                        local b = input[vertexname(edgekey2, c - 1)]
                        diff_colors = diff_colors + (a ^ b)
                    end
                    coloring = coloring * diff_colors

                    if num_colors == 3 then
                        coloring = coloring * (
                        (input[vertexname(edgekey1, 0)] + input[vertexname(edgekey1, 1)]) *
                        (input[vertexname(edgekey2, 0)] + input[vertexname(edgekey2, 1)]))
                    end
                end
            end
        end
    end
    return coloring
end

return {
    color = color
}