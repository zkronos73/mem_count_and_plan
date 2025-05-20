# Makefile per a programa multithread en C++ (executable 'pepe')

# Compilador
CXX := g++

# Opcions de compilació
CXXFLAGS := -O3 -std=c++17 -mavx2 -march=native -Wall -Wextra -pthread

# Nom de l'executable
TARGET := mem_count_and_plan

# Fitxers font
SRCS := main.cpp

# Regla per defecte
all: $(TARGET)

# Regla per compilar
$(TARGET): $(SRCS) *.hpp
	$(CXX) $(CXXFLAGS) -o $@ main.cpp

# Regla per executar
run: $(TARGET)
	./$(TARGET)

# Neteja
clean:
	rm -f $(TARGET)

.PHONY: all run clean
