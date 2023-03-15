/* Address non-standard strcasecmp()
 *
 * iot-hub-device-update needs strcasecmp(). Its signature is usually placed in
 * - Standard string.h as additional (ARMCLANG/IAR) or
 * - Non-standard strings.h (GCC)
 *
 * For iot-hub-device-update including strings.h to invoke strcasecmp(),
 * add a dummy strings.h for toolchain:
 * - Not providing strings.h, the dummy strings.h just complements.
 * - Having provided strings.h, the dummy strings.h does no harm
 *   because system directory will be searched first due to "#include <strings.h>"
 */
