import sys
import lldb_bridge

def f(a, b):
    return a + b

def g(a, b):
    a = a + 2 * b
    c = f(a, b)
    return a * b + c

def main():
    a = 1
    b = 2
    result = g(a, b)
    print(f"result = {result}")

if __name__ == "__main__":
    state = lldb_bridge.ProgramState()
    sys.settrace(state)
    main()
    sys.settrace(None)
