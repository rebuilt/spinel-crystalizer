i = 0
sum = 0
while i < 10
  i += 1
  sum += i
end
puts sum

j = 5
until j == 0
  j -= 1
end
puts j

total = 0
(1..4).each { |k| total += k * k }
puts total
