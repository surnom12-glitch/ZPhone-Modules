import sys
from lupa import luajit21
L=luajit21.LuaRuntime()
L.globals().ARG_CLIENT=sys.argv[2] if len(sys.argv)>2 else ''
L.globals().ARG_SERVER=sys.argv[3] if len(sys.argv)>3 else ''
L.execute(open(__import__('os').path.join(__import__('os').path.dirname(__file__),'harness.lua')).read())
L.execute(open(sys.argv[1]).read())
