#pragma once
#define ESP_GOTO_ON_FALSE(condition, err, label, tag, ...) do { (void)(tag); if (!(condition)) { ret = err; goto label; } } while (0)
#define ESP_RETURN_ON_FALSE(condition, err, tag, ...) do { (void)(tag); if (!(condition)) return err; } while (0)
