def gcd(a, b)
  while b != 0
    t = a % b
    a = b
    b = t
  end
  a
end
puts gcd(48, 18)
puts gcd(100, 75)
