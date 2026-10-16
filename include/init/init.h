#ifndef INIT_H
#define INIT_H

/**
 * @brief Initializes core engine components.
 * @return 0 on success, non-zero on failure.
 */
[[nodiscard]] int engine_init(void);

/**
 * @brief Cleans up and shuts down core engine components.
 */
void engine_shutdown(void);

#endif /* INIT_H */