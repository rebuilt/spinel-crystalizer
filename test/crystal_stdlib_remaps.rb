# stdlib names Crystal spells differently, and names it does not have at all.
#
# The three here all have an exact Crystal counterpart, so they are translated
# rather than refused: `Time.at` is `Time.unix` (both the same instant, but
# Ruby's is in local time and Crystal's is UTC, so the rewrite carries
# `.to_local`), `Dir.rmdir` is `Dir.delete`, and `Float::INFINITY` is the
# positive-zero divisor that has exactly that value.
p Time.at(1700000000).to_i
p Float::INFINITY > 10_000
p (1.0 / 0.0)

Dir.mkdir("crystal_stdlib_remaps_dir")
Dir.rmdir("crystal_stdlib_remaps_dir")
p Dir.glob("crystal_stdlib_remaps_dir").empty?

# `Time.at` is only translated for the one shape it matches. A fractional or
# sub-second argument has no Crystal form, and the rest of the surface must keep
# working.
p Time.now.to_i > 0
