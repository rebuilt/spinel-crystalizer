# A top-level `X = Struct.new(:a, :b)` is emitted as a Crystal reference-type
# class: a positional `initialize` assigns the members in order, and readers,
# writers, to_a/values, .members, member-wise ==, and [] (by symbol or index) are
# generated. All of these must agree with CRuby.

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
