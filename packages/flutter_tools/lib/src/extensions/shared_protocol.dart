

import 'protocol.dart';


/// A shared-isolate communication channel from an extension to the tool.
class SharedExtensionClientChannel extends ExtensionClientChannel {
  @override
  void close() {

  }

  @override
  void listen(onRequest, {Function onError, onDone}) {

  }

  @override
  void sendNotification(Notification notification) {

  }

  @override
  void sendResponse(Response response) {

  }

}

/// A shared-isolate communication channel from the tool to an extension.
class SharedExtensionServerChannel extends ExtensionServerChannel {
  @override
  void close() {
    // TODO: implement close
  }

  @override
  void kill() {
    // TODO: implement kill
  }

  @override
  void listen(onRequest, {Function onError, onDone}) {
    // TODO: implement listen
  }

  @override
  void sendRequest(Request request) {
    // TODO: implement sendRequest
  }
}
