// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/autofill/manual_fill/address.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

@implementation ManualFillAddress

- (instancetype)initWithFirstName:(NSString*)firstName
              middleNameOrInitial:(NSString*)middleNameOrInitial
                         lastName:(NSString*)lastName
                            line1:(NSString*)line1
                            line2:(NSString*)line2
                              zip:(NSString*)zip
                             city:(NSString*)city
                            state:(NSString*)state
                          country:(NSString*)country {
  self = [super init];
  if (self) {
    _firstName = [firstName copy];
    _middleNameOrInitial = [middleNameOrInitial copy];
    _lastName = [lastName copy];
    _line1 = [line1 copy];
    _line2 = [line2 copy];
    _zip = [zip copy];
    _city = [city copy];
    _state = [state copy];
    _country = [country copy];
  }
  return self;
}

- (BOOL)isEqual:(id)object {
  if (!object) {
    return NO;
  }
  if (self == object) {
    return YES;
  }
  if (![object isMemberOfClass:[ManualFillAddress class]]) {
    return NO;
  }
  ManualFillAddress* otherObject = (ManualFillAddress*)object;
  if (![otherObject.firstName isEqual:self.firstName]) {
    return NO;
  }
  if (![otherObject.middleNameOrInitial isEqual:self.middleNameOrInitial]) {
    return NO;
  }
  if (![otherObject.lastName isEqual:self.lastName]) {
    return NO;
  }
  if (![otherObject.line1 isEqual:self.line1]) {
    return NO;
  }
  if (![otherObject.line2 isEqual:self.line2]) {
    return NO;
  }
  if (![otherObject.zip isEqual:self.zip]) {
    return NO;
  }
  if (![otherObject.city isEqual:self.city]) {
    return NO;
  }
  if (![otherObject.state isEqual:self.state]) {
    return NO;
  }
  if (![otherObject.country isEqual:self.country]) {
    return NO;
  }
  return YES;
}

- (NSUInteger)hash {
  return [self.firstName hash] ^ [self.middleNameOrInitial hash] ^
         [self.lastName hash] ^ [self.line1 hash] ^ [self.line2 hash] ^
         [self.zip hash] ^ [self.city hash] ^ [self.state hash] ^
         [self.country hash];
}

- (NSString*)description {
  return
      [NSString stringWithFormat:
                    @"<%@ (%p): firstName: %@, middleNameOrInitial: %@, "
                    @"lastName: %@, line1: %@, "
                    @"line2: %@, zip: %@, city: %@, state: %@, country: %@>",
                    NSStringFromClass([self class]), self, self.firstName,
                    self.middleNameOrInitial, self.lastName, self.line1,
                    self.line2, self.zip, self.city, self.state, self.country];
}

@end
