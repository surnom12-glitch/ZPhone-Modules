import sys
from lupa import luajit21 as lj
rt = lj.LuaRuntime()
f = rt.eval("function(src, root) local fn, err = loadstring(src, '@ear.lua') if not fn then error(err) end local ok, e = xpcall(function() fn(root) end, debug.traceback) if not ok then return e end return 'ok' end")
print(f(open(sys.argv[2]).read(), sys.argv[1]))
