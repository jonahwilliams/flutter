import 'package:json_annotation/json_annotation.dart';

part 'models.g.dart';

@JsonSerializable(nullable: false)
class Person {
  Person({this.firstName, this.lastName, this.dateOfBirth});

  final String firstName;
  final String lastName;
  final DateTime dateOfBirth;

  factory Person.fromJson(Map<String, dynamic> json) => _$PersonFromJson(json);

  Map<String, dynamic> toJson() => _$PersonToJson(this);
}