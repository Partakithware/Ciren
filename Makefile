CC       = gcc
CFLAGS   = -std=gnu11 -Wall -Wextra -g \
           $(shell llvm-config-18 --cflags)
LDFLAGS  = $(shell llvm-config-18 --ldflags --libs all --system-libs)

SRCS = ciren/lexer.c \
       ciren/ast.c \
       ciren/parser.c \
       ciren/resolver.c \
       ciren/typechecker.c \
       ciren/codegen.c \
       ciren/codegen_llvm.c \
       ciren/main.c

OUT = cirenc

all: $(OUT)

$(OUT): $(SRCS)
	$(CC) $(CFLAGS) -o $(OUT) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(OUT) *.c *.ll *.o hello_world bad

.PHONY: all clean
