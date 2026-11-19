#pragma once
namespace SourcePort {
// Fatal process termination must not run normal original heap teardown again.
bool FatalDiagnosticInProgress() noexcept;
}
