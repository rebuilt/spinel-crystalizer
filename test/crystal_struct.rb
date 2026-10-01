# A top-level `X = Struct.new(:a, :b)` is emitted as a Crystal reference-type
# class: a positional `initialize` assigns the members in order, and readers,
# writers, to_a/values, .members, member-wise ==, and [] / []= (by symbol, string
# or index) are generated. All of these must agree with CRuby.

Point = Struct.new(:x, :y)
a = Point.new(1, 2)
b = Point.new(3, 4)
p a.x + b.y

a.x = 10
p [a.x, a.y]

p a == Point.new(10, 2)
p a == b
p [a] == [Point.new(10, 2)]

p a[:x]
p a[0]
p a[-1]

p a.to_a
p a.values
p Point.members

Rec = Struct.new(:name, :count)
r = Rec.new("alpha", 3)
p r.name
r.count = 7
p r.count

# []= writes a member by symbol, string name or integer position (negative counts
# from the end). Rec's members have different types, so its []= is a separate
# overload per type; each takes only the members of that type.
a[:x] = 11
p a.to_a
a["y"] = 12
p a.to_a
a[0] = 13
p a.to_a
a[-1] = 14
p a.to_a

r[:name] = "beta"
r["count"] = 9
r[0] = "gamma"
r[-1] = 10
p [r.name, r.count]
p r.to_a
