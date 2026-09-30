# A Ruby block with more params than its iterator yields auto-splats the single
# value: `each` yields one element, and `|num, sym|` binds that element's two
# elements. Crystal yields exactly one parameter, so the literal translation
# `do |num, sym|` is a build error ("too many block parameters"). The emitter
# binds one synthetic param and destructures it in the body, which also keeps
# the pair-yielding iterators (`Hash#each`, `each_with_index`) untouched.
table = [[1000, "M"], [4, "IV"], [1, "I"]]
table.each { |num, sym| p sym }

# The receiver need not be a literal: the arity rule is on the iterator, not on
# the shape of the expression it is called on.
rows = [[1, 2], [3, 4], [5, 6]]
rows.each { |a, b| p a + b }

# A one-yield iterator other than `each` desugars the same way.
p [[1, 2], [3, 4]].map { |a, b| a * b }
p [[1, 2], [3, 4]].select { |a, b| a < b }

# `Hash#each` really does yield two values, so it must keep two params and must
# NOT be desugared into a single param plus an assignment.
h = { x: 1, y: 2 }
h.each { |k, v| p [k, v] }

# `zip` yields a Tuple, which destructures through the same assignment.
[[1, 2]].zip([[3, 4]]).each { |a, b| p a[0] + b[0] }
