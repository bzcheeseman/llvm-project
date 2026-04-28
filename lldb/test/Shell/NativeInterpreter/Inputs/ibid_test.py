import sys
import lldb_bridge

def add(a, b):
    return a + b

def compute(x, y):
    z = add(x, y)
    w = add(z, x)
    return w

def main():
    p = compute(3, 4)
    q = compute(1, 2)
    return p + q

if __name__ == "__main__":
    state = lldb_bridge.ProgramState()
    sys.settrace(state)
    main()
    sys.settrace(None)
    print("done")
