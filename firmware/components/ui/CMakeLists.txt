# The input and state layer. Like the panel component it is free of ESP-IDF, so
# test/run.sh can build it with plain g++ and drive it from the simulator.
idf_component_register(
    SRCS "src/gesture.cpp"
    INCLUDE_DIRS "include"
    REQUIRES panel)
