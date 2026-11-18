/// Shared mobile business & quantitative risk logic targeting FeatherDart VM.
library shared_logic;

import 'src/ffi_exports.dart';

export 'src/analytics_engine.dart';
export 'src/ffi_exports.dart';
export 'src/host_imports.dart';

/// Default isolate entrypoint invoked by Swift / Kotlin `invokeEntrypoint("main")`.
@pragma('vm:entry-point', 'call')
void main() {
  initializeSharedLogicExports();
  print('PulseRisk shared FeatherDart engine initialized.');
}
