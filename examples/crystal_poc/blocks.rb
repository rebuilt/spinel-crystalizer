nums = [5, 10, 15, 20]
total = 0
nums.each { |n| total += n }
puts total

squared = nums.map { |n| n * n }
puts squared.join(",")

big = nums.select { |n| n > 8 }
puts big.join(",")

count = 0
nums.each do |n|
  next if n < 10
  count += 1
end
puts count
