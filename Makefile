# -Wall:     enables all basic warnings
# -Wextra:   enables extra (more strict) warnings
# -pedantic: enforces strict ISO C compliance (e.g., no compiler-specific extensions)
# -std=c99:  compiles the code using the C99 standard
pico: pico.c
	$(CC) pico.c -o pico -Wall -Wextra -pedantic -std=c99

clean:
	rm -f pico
