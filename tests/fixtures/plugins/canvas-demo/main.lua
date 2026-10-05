local c = helix.canvas("c")
local sz = helix.subject.string("sz", "")
local n = helix.subject.int("n", 0)

c:line(0, 0, 10, 10, {color = "primary"})
c:commit()

c:on_size(function(w, h)
  n:set(n:get() + 1)
  sz:set(w .. "x" .. h)
  if w >= 220 then c:on_size(nil) end
end)
