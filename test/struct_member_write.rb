# Struct#[]= writes a member by symbol, by string name, or by integer position
# (negative counts from the end), answering the stored value. An out-of-range index
# raises IndexError, which Crystal answers the same way. An unknown member NAME is a
# documented divergence instead: Ruby raises NameError and Crystal 1.21 has no such
# class, so it is left out of this agreement test.

S = Struct.new(:a, :b, :c)
s = S.new(1, 2, 3)

s[:a] = 10
p s.to_a

s["b"] = 20
p s.to_a

s[2] = 30
p s.to_a

s[-1] = 40
p s.to_a

s[-3] = 50
p s.to_a

i = 1
s[i] = 60
p s.to_a

name = "a"
s[name] = 70
p s.to_a

p (s[0] = 80)

r = (begin; s[9] = 1; "no error"; rescue => e; e.class; end)
p r

r2 = (begin; s[-4] = 1; "no error"; rescue => e; e.class; end)
p r2

p s.to_a

# A heterogeneous struct gets one []= overload per member type, so a write never
# needs a union or a cast.
Rec = Struct.new(:name, :count)
rec = Rec.new("alpha", 3)
rec[:name] = "beta"
rec["count"] = 9
rec[0] = "gamma"
rec[-1] = 11
p [rec.name, rec.count]
p rec.to_a
