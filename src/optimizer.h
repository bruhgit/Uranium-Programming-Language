#ifndef uranium_optimizer_h
#define uranium_optimizer_h

#include "value.h"

struct FastPathPlan;

void optimizeFunctionTree(const FunctionPtr& function, int level);
bool buildFastPathPlan(const FunctionPtr& function,
                       FastPathPlan* plan,
                       std::string* reason);

#endif
