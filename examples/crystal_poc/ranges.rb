sum = 0
(1..5).each { |v| sum += v }
puts sum

excl = 0
(1...4).each { |v| excl += v }
puts excl

mapped = (1..3).map { |v| v * 10 }
puts mapped.join(",")
