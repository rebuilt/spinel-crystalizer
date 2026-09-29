nums = [1, 2, 3]
total = 0
nums.each do |a|
  nums.each do |b|
    total += a * b
  end
end
puts total
