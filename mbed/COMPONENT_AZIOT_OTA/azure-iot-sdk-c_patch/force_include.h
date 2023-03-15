/*
 * This file and <SUB_DIRECTORY>/.mbedignore force the Mbed OS build system to
 *   - pass to the compiler -I<this directory>
 *   - NOT pass to the compiler -I<THIS_DIRECTORY>/<SUB_DIRECTORY>
 * so that
 *   - "#include <HEADER>" resolves to non-<THIS_DIRECTORY>/<SUB_DIRECTORY>/<HEADER>
 *   - "#include <SUB_DIRECTORY>/<HEADER> resolves to <THIS_DIRECTORY>/<SUB_DIRECTORY>/<HEADER>
 */
