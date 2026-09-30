# Locals whose names are Crystal keywords. `out`, `in`, `as` and `with` are
# ordinary identifiers in Ruby but reserved in Crystal, so emitting them verbatim
# produced `out = ""` and a parse error ("expecting variable or instance variable
# after out"). The emitter renames such a local consistently at every binding
# site, so the program still means the same thing.
table = [[1000, 1], [4, 2], [1, 3]]
out = ""
table.each { |num, sym| out += num.to_s }
p out

# The other Crystal-only keywords that are legal Ruby identifiers.
as = 0
as += 5
as *= 3
p as

# A block param named after a Crystal keyword renames with the local.
with = ["x", "y"]
with.each { |with| p with.upcase }

# And a method parameter named after a Crystal keyword renames too, and stays
# distinct from the caller's local of the same Ruby name (Crystal's `def` params
# are function-scoped, so both may exist).
def widen(with)
  with * 10
end
p widen(4)
