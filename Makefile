# Makefile per a programa multithread en C++ (executable 'pepe')

# Compilador
CXX := g++

# Opcions de compilació
CXXFLAGS := -g -O3 -std=c++17 -Wall -Wextra -pthread

# Nom de l'executable
TARGET := mem_count_and_plan

# Fitxers font
SRCS := main.cpp

# Regla per defecte
all: $(TARGET)

# Regla per compilar
$(TARGET): $(SRCS) *.hpp
	$(CXX) $(CXXFLAGS) -o $@ $^

# Regla per executar
run: $(TARGET)
	./$(TARGET)

# Neteja
clean:
	rm -f $(TARGET)

.PHONY: all run clean
