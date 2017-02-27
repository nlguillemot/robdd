local coloring = require 'coloring'

n = 3

title = tostring(n) .. '-colorings of the Petersen graph\'s vertices'

connections = {
     [1] = {2,5,7},
     [2] = {1,3,8},
     [3] = {2,4,9},
     [4] = {3,5,10},
     [5] = {1,4,6},
     [6] = {5,8,9},
     [7] = {1,9,10},
     [8] = {2,6,10},
     [9] = {3,6,7},
    [10] = {4,7,8}
}

output.coloring = coloring.color(connections, n)
