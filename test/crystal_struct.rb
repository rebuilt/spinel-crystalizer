# A top-level `X = Struct.new(:a, :b)` is emitted as a Crystal reference-type
# class: a positional `initialize` assigns the members in order, and a reader and
# writer are generated for each member. This exercises the constructor, member
# reads, in-place writes, multiple member types (Int and String), and passing
# instances around in an array -- all of which must agree with CRuby.

Point = Struct.new(:x, :y)
p Point.new(1, 2).x + Point.new(3, 4).y

a = Point.new(10, 20)
a.x = 99
p [a.x, a.y]

Rec = Struct.new(:name, :count)
r = Rec.new("alpha", 3)
p r.name
r.count = 7
p r.count

pts = [Point.new(1, 1), Point.new(2, 2)]
total = 0
pts.each { |pt| total += pt.x + pt.y }
p total
