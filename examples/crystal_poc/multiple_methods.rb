def add(a, b)
  a + b
end
def mul(a, b)
  a * b
end
def combine(a, b)
  add(mul(a, b), 1)
end
puts combine(3, 4)
