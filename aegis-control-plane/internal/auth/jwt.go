package auth

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"net/http"
	"strings"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/google/uuid"
)

type contextKey string

const userIDKey contextKey = "user_id"
const sessionIDKey contextKey = "session_id"

type Claims struct {
	UserID    string `json:"uid"`
	SessionID string `json:"sid,omitempty"`
	jwt.RegisteredClaims
}

type SessionValidator func(ctx context.Context, sessionID, userID string) error

func Middleware(secret string, validateSession SessionValidator) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			authz := r.Header.Get("Authorization")
			if authz == "" || !strings.HasPrefix(authz, "Bearer ") {
				http.Error(w, `{"error":{"code":"unauthorized","message":"missing bearer token"}}`, http.StatusUnauthorized)
				return
			}

			tokenRaw := strings.TrimSpace(strings.TrimPrefix(authz, "Bearer "))
			claims := &Claims{}
			token, err := jwt.ParseWithClaims(tokenRaw, claims, func(token *jwt.Token) (any, error) {
				if _, ok := token.Method.(*jwt.SigningMethodHMAC); !ok {
					return nil, errors.New("unexpected signing method")
				}
				return []byte(secret), nil
			})
			if err != nil || !token.Valid || claims.UserID == "" {
				http.Error(w, `{"error":{"code":"unauthorized","message":"invalid token"}}`, http.StatusUnauthorized)
				return
			}

			ctx := context.WithValue(r.Context(), userIDKey, claims.UserID)
			if claims.SessionID != "" {
				if validateSession == nil || validateSession(r.Context(), claims.SessionID, claims.UserID) != nil {
					http.Error(w, `{"error":{"code":"unauthorized","message":"invalid token"}}`, http.StatusUnauthorized)
					return
				}
				ctx = context.WithValue(ctx, sessionIDKey, claims.SessionID)
			}
			next.ServeHTTP(w, r.WithContext(ctx))
		})
	}
}

func UserIDFromContext(ctx context.Context) (string, bool) {
	v := ctx.Value(userIDKey)
	s, ok := v.(string)
	return s, ok && s != ""
}

func SessionIDFromContext(ctx context.Context) (string, bool) {
	v := ctx.Value(sessionIDKey)
	s, ok := v.(string)
	return s, ok && s != ""
}

func SignSessionJWT(secret, userID, sessionID string, ttl time.Duration) (string, error) {
	now := time.Now().UTC()
	claims := Claims{
		UserID:    userID,
		SessionID: sessionID,
		RegisteredClaims: jwt.RegisteredClaims{
			ExpiresAt: jwt.NewNumericDate(now.Add(ttl)),
			IssuedAt:  jwt.NewNumericDate(now),
			ID:        uuid.NewString(),
		},
	}
	tok := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	return tok.SignedString([]byte(secret))
}

func GenerateOpaqueToken(bytesLen int) (string, error) {
	b := make([]byte, bytesLen)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

func HashOpaqueToken(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}
