(provide 'klee-pc-mode)

(require 'font-lock)

(defvar klee-pc-mode-syntax-table nil 
  "Syntax table used while in ft-bt mode.")

;;; --------------------------------------------------
;;;setup the syntax table
;;; --------------------------------------------------
(unless klee-pc-mode-syntax-table
  (setq klee-pc-mode-syntax-table (make-syntax-table))
  ;;misc
  (modify-syntax-entry ?\_   "w"       klee-pc-mode-syntax-table)
  (modify-syntax-entry ?\.   "."       klee-pc-mode-syntax-table))

;;;------------------------------------------------------------
;;; local keymap
;;;------------------------------------------------------------

(defvar klee-pc-local-keymap nil)

;;; --------------------------------------------------
;;; set keywords
;;; --------------------------------------------------

(defvar klee-pc-mode-font-lock-keywords
  (list
   ;; Comments
   '("#.*" . font-lock-comment-face)
   ;; Identifiers
   '("%[_a-zA-Z][a-zA-Z_.0-9]*" . font-lock-variable-name-face)
   ;; Numbers
   '("[+-]?0b[01_]+" . font-lock-preprocessor-face)
   '("[+-]?0o[0-7_]+" . font-lock-preprocessor-face)
   '("[+-]?0x[a-zA-Z0-9_]+" . font-lock-preprocessor-face)
   '("[+-]?[0-9]+" . font-lock-preprocessor-face)
   ;; Keywords
   '("\\bdef\\b\\|\\bvar\\b\\|\\btrue\\b\\|\\barray\\b\\|\\bfalse\\b\\|\\bquery\\b\\|\\bdefine\\b\\|\\bdeclare\\b" . font-lock-keyword-face)
   )
  "klee-PC mode keywords")

;;; --------------------------------------------------
;;; major mode function
;;; --------------------------------------------------
(defun klee-pc-mode ()
  "Switch to the klee-pc major mode"
  (interactive)
  (kill-all-local-variables)
  (setq major-mode 'klee-pc-mode)
  (setq mode-name "klee-pc")

  ;; handle keymap
  (use-local-map klee-pc-local-keymap)

  ;;handle syntax table
  (set-syntax-table klee-pc-mode-syntax-table)
  ;;handle fontlock
  (make-local-variable 'font-lock-defaults)
  (setq font-lock-defaults '(klee-pc-mode-font-lock-keywords
			     nil t))

  ;;handle hook
  (run-hooks 'klee-pc-mode-hook))
