a = [1, 2, 3, 4]
puts a.length
puts a[0]
puts a[-1]
b = []
b << 10
b << 20
puts b.join(",")
doubled = a.map { |x| x * 2 }
puts doubled.join(",")
evens = a.select { |x| x % 2 == 0 }
puts evens.join(",")
puts a.sum
