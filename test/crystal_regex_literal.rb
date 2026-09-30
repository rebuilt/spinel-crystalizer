# A Ruby /pattern/ literal is Crystal's native regex literal, same syntax: the
# pattern text drops in verbatim and the trailing option flags (/i /m /x) carry
# over. This exercises the plain form, a flag, case/when dispatch on a regex,
# MatchData accessors, and gsub -- all of which must agree with CRuby.

p "hello world".match(/l+/) ? "yes" : "no"
p "abc123def".sub(/\d+/, "N")
p "HELLO".match(/hello/i) ? "yes" : "no"

m = "a-b-c".match(/b/)
p m ? (m.pre_match + m.post_match) : "none"

case "hi there"
when /^h/ then p "starts-h"
when /world/ then p "has-world"
else p "other"
end

p "x1y2z3".gsub(/[0-9]/, "#")
