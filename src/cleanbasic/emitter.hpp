#ifndef CLEANBASIC_EMITTER_HPP
#define CLEANBASIC_EMITTER_HPP

#include "src/cleanbasic/analyzer.hpp"

namespace cb {

inline std::string emitProgram(
    const AstArena& /*arena*/,
    const GlobalSymbolTable& symbols,
    const Program& /*program*/
) {
    return symbols.emittedCpp;
}

} // namespace cb

#endif // CLEANBASIC_EMITTER_HPP
