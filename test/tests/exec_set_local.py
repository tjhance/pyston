def f():
    exec "a = 5"
    print a
f()

def f():
    a = 0
    exec "a = 5"
    print a
f()

def f():
    a = 0
    exec "del a"
    print a
f()

def f():
    a = 0
    del a
    exec "a = 4"
    print a
f()
