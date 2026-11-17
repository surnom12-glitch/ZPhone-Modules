import sys
from lupa import luajit21 as lj
rt = lj.LuaRuntime()
root = sys.argv[1]
src = open(sys.argv[2]).read()
f = rt.eval("function(src, root) local fn, err = loadstring(src, '@harness.lua') if not fn then error(err) end local ok, e = xpcall(function() fn(root) end, debug.traceback) if not ok then return e end return 'ok' end")
print(f(src, root))
