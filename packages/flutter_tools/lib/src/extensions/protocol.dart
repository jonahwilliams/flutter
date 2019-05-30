
/// A communication channel from an extension to the tool.
abstract class ExtensionClientChannel {
  void close();

  void listen(void Function(Request) onRequest, {Function onError, void Function() onDone});

  void sendNotification(Notification notification);

  void sendResponse(Response response);
}

/// A communication channel from the tool to an extension.
abstract class ExtensionServerChannel {
  void close();

  /// Immediately terminate the extension.
  void kill();

  void listen(void Function(Request) onRequest, {Function onError, void Function() onDone});

  void sendRequest(Request request);
}

/// A communication from the flutter tool that does not expect a response.
class Notification {
  const Notification(this.event, this.params);

  static const String _kEvent = 'event';
  static const String _kParams = 'params';

  /// The name of the notification.
  final String event;

  /// Dynamic parameters containing notification data.
  final Map<String, Object> params;

  /// Convert the notification object into JSON.
  Map<String, Object> toJson() {
    final Map<String, Object> result = <String, Object>{};
    result[_kEvent] = event;
    if (params != null) {
      result[_kParams] = params;
    }
    return result;
  }
}

/// A communication from flutter tool that expects a corresponding [Response].
///
/// The matching response will have an identical [id] field.
class Request {
  const Request(this.id, this.method, this.params);

  static const String _kId = 'id';
  static const String _kRequestTime = 'requestTime';
  static const String _kResult = 'result';
  static const String _kError = 'error';

  final String id;
  final String method;
  final Map<String, Object> params;

  /// Convert the request to a JSON serializable object.
  Map<String, Object> toJson() {
    final Map<String, Object> result = <String, Object>{};
    result[_kId] = id;
    if (result != null) {
      result[_kResult] = result;
    }
    return result;
  }
}

/// A response contains data returned from the extension from the flutter tool.
class Response {
  Response(this.id, this.requestTime, {this.result, this.error});

  /// Create a response from a blob of JSON.
  factory Response.fromJson(Map<String, dynamic> json) {
    final String id = json[_kId];
    final int requestTime = json[_kRequestTime];
    final Map<String, Object> result = json[_kResult];
    final Map<String, Object> error = json[_kError];
    return Response(
      id, requestTime, result: result, error: error,
    );
  }

  static const String _kId = 'id';
  static const String _kRequestTime = 'requestTime';
  static const String _kResult = 'result';
  static const String _kError = 'error';

  /// A unique identifier for the response.
  final String id;

  /// The time in milliseconds since that the response was created.
  final int requestTime;

  /// Dynamic parameters containing the result information.
  ///
  /// May be null if there was an error.
  final Map<String, Object> result;

  /// Dynamic parameters containing any error information.
  ///
  /// May be null if there was no error.
  final Map<String, Object> error;

  /// Convert the response to a JSON serializable object.
  Map<String, Object> toJson() {
    final Map<String, Object> result = <String, Object>{};
    result[_kId] = id;
    result[_kRequestTime] = requestTime;
    if (error != null) {
      result[_kError] = error;
    }
    if (result != null) {
      result[_kResult] = result;
    }
    return result;
  }
}
