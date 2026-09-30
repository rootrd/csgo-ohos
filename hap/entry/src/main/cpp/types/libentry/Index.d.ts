export const setGameRoot: (gameRoot: string, filesDir: string) => boolean;
export const setDisplaySize: (width: number, height: number) => boolean;
export const prepareGameEnv: () => boolean;
export const pingGame: () => Record<string, Object>;