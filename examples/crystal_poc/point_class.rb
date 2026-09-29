class Point
  attr_accessor :x, :y
  def initialize(x, y)
    @x = x
    @y = y
  end
  def dist
    Math.sqrt(@x * @x + @y * @y)
  end
end
pts = []
10.times { |i| pts << Point.new(i, i * 2) }
total = 0.0
pts.each { |p| total += p.dist }
puts total
