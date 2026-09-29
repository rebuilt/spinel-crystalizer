def classify(n)
  if n < 0
    "neg"
  elsif n == 0
    "zero"
  else
    "pos"
  end
end
puts classify(-3)
puts classify(0)
puts classify(7)

x = 10
unless x > 100
  puts "small"
end

flag = true
r = flag ? "yes" : "no"
puts r
